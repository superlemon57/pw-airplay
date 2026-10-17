/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - paced PCM feed into the OwnTone pipe */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pipewire/pipewire.h>

#define ENGINE_RATE 44100
#define ENGINE_CHANNELS 2
#define ENGINE_FRAME_BYTES 4

struct engine;

struct engine_stats {
	uint64_t frames_out;
	uint64_t frames_silence;	/* inserted because the ring ran dry */
	uint64_t frames_dropped;	/* excess trimmed, or the pipe was full */
	uint32_t underruns;
	uint32_t level_frames;		/* current ring level */
	bool running;
	bool broken;			/* reader went away, needs a restart */
};

/* data_loop is the loop the sinks' process callbacks run on */
struct engine *engine_new(struct pw_loop *data_loop, const char *fifo_path);
void engine_free(struct engine *e);

/* Main thread. Opens the pipe and starts the paced feed (silence until an
 * owner pushes audio). Returns -errno, e.g. -ENXIO when OwnTone isn't reading. */
int engine_start(struct engine *e);
void engine_stop(struct engine *e);
bool engine_running(struct engine *e);

/* Main thread. Only audio pushed by the owner reaches the speakers */
void engine_set_owner(struct engine *e, void *owner);
void engine_get_stats(struct engine *e, struct engine_stats *stats);

/* Ring level the pacer aims for, i.e. the latency the engine itself adds */
uint32_t engine_target_level_ms(void);

/* Data thread. S16LE stereo 44.1 kHz */
void engine_push(struct engine *e, void *owner, const void *data, uint32_t bytes);
