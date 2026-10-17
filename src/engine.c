/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - paced PCM feed into the OwnTone pipe
 *
 * OwnTone timestamps audio by the moment it reads it from the pipe, so the end
 * to end latency is only constant if the pipe is fed at a constant rate. The
 * sinks therefore don't write to the pipe directly: they push into a ring, and
 * a pacer on CLOCK_MONOTONIC (the clock OwnTone's player runs on) writes
 * exactly ENGINE_RATE frames per second, filling gaps with silence and
 * trimming any excess. Everything except start/stop runs on the data loop.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spa/utils/ringbuffer.h>

#include "util.h"
#include "engine.h"

#define RING_BYTES (1u << 18)				/* ~1.5 s */
#define PACER_INTERVAL_NS (5 * SPA_NSEC_PER_MSEC)
/* Ring cushion against the graph's quantum delivery jitter */
#define TARGET_LEVEL_FRAMES (ENGINE_RATE * 40 / 1000)
#define MAX_LEVEL_FRAMES (ENGINE_RATE * 120 / 1000)
/* Writes up to PIPE_BUF are atomic, so we never write half a frame */
#define WRITE_CHUNK 4096
#define PIPE_SIZE (16 * 1024)

struct engine {
	struct pw_loop *loop;
	struct spa_source *pacer;
	char *fifo_path;

	/* Owned by the data thread once started */
	int fd;
	void *owner;
	struct spa_ringbuffer ring;
	uint8_t *buf;
	uint64_t t0;
	uint64_t frames_out;
	bool primed;

	_Atomic uint64_t st_frames_out;
	_Atomic uint64_t st_silence;
	_Atomic uint64_t st_dropped;
	_Atomic uint32_t st_underruns;
	_Atomic uint32_t st_level;
	_Atomic bool broken;
	bool running;	/* main thread view */
};

static uint8_t silence[WRITE_CHUNK];

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

uint32_t engine_target_level_ms(void)
{
	/* Average ring level is the target plus half a pacer interval */
	return TARGET_LEVEL_FRAMES * 1000 / ENGINE_RATE + PACER_INTERVAL_NS / SPA_NSEC_PER_MSEC / 2;
}

/* Returns false if the reader is gone */
static bool pipe_write(struct engine *e, const uint8_t *data, uint32_t bytes)
{
	while (bytes > 0) {
		uint32_t len = SPA_MIN(bytes, (uint32_t)WRITE_CHUNK);
		ssize_t res = write(e->fd, data, len);

		if (res < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				/* OwnTone isn't keeping up. Drop, but stay on the clock */
				atomic_fetch_add(&e->st_dropped, bytes / ENGINE_FRAME_BYTES);
				return true;
			}
			return false;
		}
		data += res;
		bytes -= res;
	}
	return true;
}

static bool write_silence(struct engine *e, uint32_t frames)
{
	uint32_t bytes = frames * ENGINE_FRAME_BYTES;

	while (bytes > 0) {
		uint32_t len = SPA_MIN(bytes, (uint32_t)sizeof(silence));
		if (!pipe_write(e, silence, len))
			return false;
		bytes -= len;
	}
	atomic_fetch_add(&e->st_silence, frames);
	return true;
}

static bool write_ring(struct engine *e, uint32_t index, uint32_t frames)
{
	uint32_t bytes = frames * ENGINE_FRAME_BYTES;
	uint32_t offs = index & (RING_BYTES - 1);
	uint32_t first = SPA_MIN(bytes, RING_BYTES - offs);

	if (!pipe_write(e, e->buf + offs, first))
		return false;
	if (bytes > first && !pipe_write(e, e->buf, bytes - first))
		return false;
	spa_ringbuffer_read_update(&e->ring, index + bytes);
	return true;
}

static void on_pacer(void *data, uint64_t expirations)
{
	struct engine *e = data;
	uint64_t now, target;
	uint32_t need, index, avail, take;
	int32_t filled;
	bool ok = true;

	if (e->fd < 0)
		return;

	now = now_ns();
	target = (now - e->t0) * ENGINE_RATE / SPA_NSEC_PER_SEC;
	if (target <= e->frames_out)
		return;

	if (target - e->frames_out > ENGINE_RATE) {
		/* We were stalled for over a second (e.g. system suspend). Don't
		 * try to catch up, just continue from now. */
		e->t0 = now;
		e->frames_out = 0;
		return;
	}
	need = target - e->frames_out;

	filled = spa_ringbuffer_get_read_index(&e->ring, &index);
	avail = filled > 0 ? (uint32_t)filled / ENGINE_FRAME_BYTES : 0;

	if (!e->primed && avail >= TARGET_LEVEL_FRAMES)
		e->primed = true;

	if (e->primed) {
		if (avail > MAX_LEVEL_FRAMES + need) {
			/* Keep the added latency bounded */
			uint32_t skip = avail - TARGET_LEVEL_FRAMES - need;
			index += skip * ENGINE_FRAME_BYTES;
			spa_ringbuffer_read_update(&e->ring, index);
			avail -= skip;
			atomic_fetch_add(&e->st_dropped, skip);
		}
		take = SPA_MIN(avail, need);
		ok = write_ring(e, index, take);
		if (ok && take < need) {
			e->primed = false;
			atomic_fetch_add(&e->st_underruns, 1);
			ok = write_silence(e, need - take);
		}
		avail -= take;
	} else {
		ok = write_silence(e, need);
	}

	e->frames_out += need;
	atomic_fetch_add(&e->st_frames_out, need);
	atomic_store(&e->st_level, avail);

	if (!ok) {
		pw_log_warn("pipe reader went away: %m");
		close(e->fd);
		e->fd = -1;
		atomic_store(&e->broken, true);
	}
}

struct engine *engine_new(struct pw_loop *data_loop, const char *fifo_path)
{
	struct engine *e = xcalloc(1, sizeof(*e));

	e->loop = data_loop;
	e->fifo_path = xstrdup(fifo_path);
	e->fd = -1;
	e->buf = xcalloc(1, RING_BYTES);
	spa_ringbuffer_init(&e->ring);
	e->pacer = pw_loop_add_timer(e->loop, on_pacer, e);
	return e;
}

void engine_free(struct engine *e)
{
	if (!e)
		return;
	engine_stop(e);
	pw_loop_destroy_source(e->loop, e->pacer);
	free(e->buf);
	free(e->fifo_path);
	free(e);
}

static int do_start(struct spa_loop *loop, bool async, uint32_t seq,
		    const void *data, size_t size, void *user_data)
{
	struct engine *e = user_data;
	struct timespec value, interval;

	e->fd = *(const int *)data;
	e->t0 = now_ns();
	e->frames_out = 0;
	e->primed = false;
	spa_ringbuffer_init(&e->ring);
	atomic_store(&e->broken, false);

	value.tv_sec = 0;
	value.tv_nsec = PACER_INTERVAL_NS;
	interval = value;
	pw_loop_update_timer(e->loop, e->pacer, &value, &interval, false);
	return 0;
}

int engine_start(struct engine *e)
{
	int fd;

	if (e->running)
		return 0;

	/* Fails with ENXIO if OwnTone doesn't have the pipe open */
	fd = open(e->fifo_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	/* A small pipe bounds how much can pile up if OwnTone stalls */
	fcntl(fd, F_SETPIPE_SZ, PIPE_SIZE);

	pw_loop_invoke(e->loop, do_start, 0, &fd, sizeof(fd), true, e);
	e->running = true;
	return 0;
}

static int do_stop(struct spa_loop *loop, bool async, uint32_t seq,
		   const void *data, size_t size, void *user_data)
{
	struct engine *e = user_data;
	int *fd = *(int *const *)data;

	pw_loop_update_timer(e->loop, e->pacer, NULL, NULL, false);
	*fd = e->fd;
	e->fd = -1;
	e->owner = NULL;
	return 0;
}

void engine_stop(struct engine *e)
{
	int fd = -1, *fdp = &fd;

	if (!e->running)
		return;

	/* pw_loop_invoke copies data in, so pass a pointer to get the fd out.
	 * Closing the pipe makes OwnTone stop playback (autostop on EOF). */
	pw_loop_invoke(e->loop, do_stop, 0, &fdp, sizeof(fdp), true, e);
	if (fd >= 0)
		close(fd);
	e->running = false;
}

bool engine_running(struct engine *e)
{
	return e->running;
}

static int do_set_owner(struct spa_loop *loop, bool async, uint32_t seq,
			const void *data, size_t size, void *user_data)
{
	struct engine *e = user_data;

	e->owner = *(void *const *)data;
	return 0;
}

void engine_set_owner(struct engine *e, void *owner)
{
	pw_loop_invoke(e->loop, do_set_owner, 0, &owner, sizeof(owner), true, e);
}

void engine_get_stats(struct engine *e, struct engine_stats *stats)
{
	stats->frames_out = atomic_load(&e->st_frames_out);
	stats->frames_silence = atomic_load(&e->st_silence);
	stats->frames_dropped = atomic_load(&e->st_dropped);
	stats->underruns = atomic_load(&e->st_underruns);
	stats->level_frames = atomic_load(&e->st_level);
	stats->running = e->running;
	stats->broken = atomic_load(&e->broken);
}

void engine_push(struct engine *e, void *owner, const void *data, uint32_t bytes)
{
	uint32_t index;
	int32_t filled;

	if (owner != e->owner || e->fd < 0)
		return;

	bytes -= bytes % ENGINE_FRAME_BYTES;
	filled = spa_ringbuffer_get_write_index(&e->ring, &index);
	if (filled < 0 || (uint32_t)filled + bytes > RING_BYTES) {
		atomic_fetch_add(&e->st_dropped, bytes / ENGINE_FRAME_BYTES);
		return;
	}
	spa_ringbuffer_write_data(&e->ring, e->buf, RING_BYTES,
				  index & (RING_BYTES - 1), data, bytes);
	spa_ringbuffer_write_update(&e->ring, index + bytes);
}
