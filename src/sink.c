/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - a PipeWire Audio/Sink for one AirPlay target (device or group)
 *
 * The sink drives its own graph from a CLOCK_MONOTONIC timer, the same clock
 * the pacer in engine.c and OwnTone's player use, so there is no drift to
 * correct. Volume is not applied to the samples but sent to the speakers, like
 * macOS does. The end to end AirPlay latency is reported to PipeWire as
 * process latency, which is what lets players delay video to match.
 */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include "util.h"
#include "sink.h"

/* Anything above this counts as audio, below is silence or dither */
#define SILENCE_THRESHOLD 8

struct sink {
	struct pw_core *core;
	struct pw_loop *data_loop;
	struct engine *engine;
	const struct sink_events *events;
	void *events_data;

	char *key;
	char *description;
	char **ids;
	int n_ids;

	struct pw_stream *stream;
	struct spa_hook stream_listener;
	enum pw_stream_state state;

	/* Data thread */
	struct spa_source *timer;
	struct spa_io_position *position;
	uint64_t next_time;

	_Atomic uint64_t last_audio_ns;

	struct spa_process_latency_info process_latency;
	float volume;	/* linear, as PipeWire reports it */
	bool mute;
	int sent_pct;
	/* Speaker volume to apply once the stream accepts controls, or -1 */
	int pending_pct;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

/* PipeWire volumes are linear, UIs show the cube root. So does AirPlay's
 * -30..0 dB range, which OwnTone maps from 0-100. */
static int volume_to_pct(float volume, bool mute)
{
	if (mute)
		return 0;
	return (int)lroundf(SPA_CLAMPF(cbrtf(volume), 0.0f, 1.0f) * 100.0f);
}

static float pct_to_volume(int pct)
{
	float v = SPA_CLAMPF(pct / 100.0f, 0.0f, 1.0f);
	return v * v * v;
}

static void set_timeout(struct sink *sink, uint64_t time)
{
	struct timespec value, interval = { 0, 0 };

	value.tv_sec = time / SPA_NSEC_PER_SEC;
	value.tv_nsec = time % SPA_NSEC_PER_SEC;
	pw_loop_update_timer(sink->data_loop, sink->timer, &value, &interval, true);
}

/* Data thread: we are the driver, start a graph cycle */
static void on_timeout(void *data, uint64_t expirations)
{
	struct sink *sink = data;
	struct spa_io_position *pos = sink->position;
	uint64_t duration = 1024, current;
	uint32_t rate = 48000;

	if (SPA_LIKELY(pos)) {
		duration = pos->clock.target_duration;
		rate = pos->clock.target_rate.denom;
	}

	current = sink->next_time;
	sink->next_time += duration * SPA_NSEC_PER_SEC / rate;

	if (SPA_LIKELY(pos)) {
		pos->clock.nsec = current;
		pos->clock.rate = pos->clock.target_rate;
		pos->clock.position += pos->clock.duration;
		pos->clock.duration = pos->clock.target_duration;
		pos->clock.delay = 0;
		pos->clock.rate_diff = 1.0;
		pos->clock.next_nsec = sink->next_time;
	}
	set_timeout(sink, sink->next_time);

	pw_stream_trigger_process(sink->stream);
}

static int do_driver_start(struct spa_loop *loop, bool async, uint32_t seq,
			   const void *data, size_t size, void *user_data)
{
	struct sink *sink = user_data;

	sink->next_time = now_ns();
	set_timeout(sink, sink->next_time);
	return 0;
}

static int do_driver_stop(struct spa_loop *loop, bool async, uint32_t seq,
			  const void *data, size_t size, void *user_data)
{
	struct sink *sink = user_data;

	set_timeout(sink, 0);
	return 0;
}

static bool has_audio(const int16_t *samples, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (samples[i] > SILENCE_THRESHOLD || samples[i] < -SILENCE_THRESHOLD)
			return true;
	return false;
}

/* Data thread */
static void on_process(void *data)
{
	struct sink *sink = data;
	struct pw_buffer *b;
	struct spa_data *d;
	uint32_t offs, size;
	const uint8_t *ptr;

	if ((b = pw_stream_dequeue_buffer(sink->stream)) == NULL)
		return;

	d = &b->buffer->datas[0];
	if (d->data && d->chunk) {
		offs = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->chunk->size, d->maxsize - offs);
		ptr = SPA_PTROFF(d->data, offs, const uint8_t);

		if (has_audio((const int16_t *)ptr, size / sizeof(int16_t)))
			atomic_store(&sink->last_audio_ns, now_ns());

		engine_push(sink->engine, sink, ptr, size);
	}

	pw_stream_queue_buffer(sink->stream, b);
}

static void on_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct sink *sink = data;

	if (id == SPA_IO_Position)
		sink->position = area;
}

static void update_latency_params(struct sink *sink)
{
	struct spa_latency_info latency = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	if (!sink->stream)
		return;

	/* Same approach as PipeWire's own network sinks (module-rtp/stream.c) */
	spa_process_latency_info_add(&sink->process_latency, &latency);
	params[0] = spa_latency_build(&b, SPA_PARAM_Latency, &latency);
	params[1] = spa_process_latency_build(&b, SPA_PARAM_ProcessLatency, &sink->process_latency);
	pw_stream_update_params(sink->stream, params, 2);
}

void sink_set_device_volume(struct sink *sink, int pct);

static void on_state_changed(void *data, enum pw_stream_state old,
			     enum pw_stream_state state, const char *error)
{
	struct sink *sink = data;

	sink->state = state;

	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		if (pw_stream_is_driving(sink->stream))
			pw_loop_invoke(sink->data_loop, do_driver_start, 0, NULL, 0, true, sink);
		break;
	case PW_STREAM_STATE_PAUSED:
		pw_loop_invoke(sink->data_loop, do_driver_stop, 0, NULL, 0, true, sink);
		if (old == PW_STREAM_STATE_CONNECTING) {
			update_latency_params(sink);
			if (sink->pending_pct >= 0) {
				int pct = sink->pending_pct;

				sink->pending_pct = -1;
				sink_set_device_volume(sink, pct);
			}
		}
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_error("sink '%s': %s", sink->description, error ? error : "error");
		pw_loop_invoke(sink->data_loop, do_driver_stop, 0, NULL, 0, true, sink);
		break;
	default:
		break;
	}
}

/* Intercepts volume changes so they go to the speakers instead of being
 * applied to the samples, like module-raop-sink */
static void on_props_changed(struct sink *sink, uint32_t id, const struct spa_pod *param)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f;
	struct spa_pod_object *obj = (struct spa_pod_object *)param;
	struct spa_pod_prop *prop;
	bool changed = false;
	int pct;

	spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_mute:
		{
			bool mute;

			if (spa_pod_get_bool(&prop->value, &mute) == 0 && mute != sink->mute) {
				sink->mute = mute;
				changed = true;
			}
			spa_pod_builder_prop(&b, SPA_PROP_softMute, 0);
			spa_pod_builder_bool(&b, false);
			spa_pod_builder_raw_padded(&b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
		case SPA_PROP_channelVolumes:
		{
			float vols[SPA_AUDIO_MAX_CHANNELS], soft[SPA_AUDIO_MAX_CHANNELS], volume = 0.0f;
			uint32_t n;

			n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, vols, SPA_N_ELEMENTS(vols));
			if (n > 0) {
				for (uint32_t i = 0; i < n; i++) {
					volume += vols[i];
					soft[i] = 1.0f;
				}
				volume /= n;
				if (volume != sink->volume) {
					sink->volume = volume;
					changed = true;
				}
				spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
				spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, n, soft);
			}
			spa_pod_builder_raw_padded(&b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
		case SPA_PROP_softVolumes:
		case SPA_PROP_softMute:
			break;
		default:
			spa_pod_builder_raw_padded(&b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
	}
	param = spa_pod_builder_pop(&b, &f);
	pw_stream_set_param(sink->stream, id, param);

	pct = volume_to_pct(sink->volume, sink->mute);
	if (changed && pct != sink->sent_pct) {
		sink->sent_pct = pct;
		if (sink->events && sink->events->volume_changed)
			sink->events->volume_changed(sink->events_data, sink, pct);
	}
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct sink *sink = data;

	switch (id) {
	case SPA_PARAM_Props:
		if (param)
			on_props_changed(sink, id, param);
		break;
	case SPA_PARAM_ProcessLatency:
		/* Keep ours, a session manager may try to reset it */
		update_latency_params(sink);
		break;
	default:
		break;
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.io_changed = on_io_changed,
	.process = on_process,
};

static char *make_node_name(const char *key)
{
	char *name = xmalloc(strlen(key) + 32), *p;

	sprintf(name, "airplay2_sink.%s", key);
	for (p = name + strlen("airplay2_sink."); *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-'))
			*p = '_';
	return name;
}

struct sink *sink_new(struct pw_core *core, struct pw_loop *data_loop, struct engine *engine,
		      const char *key, const char *description, uint32_t latency_ms,
		      const struct sink_events *events, void *data)
{
	struct sink *sink = xcalloc(1, sizeof(*sink));
	struct spa_audio_info_raw info = {
		.format = SPA_AUDIO_FORMAT_S16_LE,
		.rate = ENGINE_RATE,
		.channels = ENGINE_CHANNELS,
		.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
	};
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_properties *props;
	char *node_name;
	int res;

	sink->core = core;
	sink->data_loop = data_loop;
	sink->engine = engine;
	sink->events = events;
	sink->events_data = data;
	sink->key = xstrdup(key);
	sink->description = xstrdup(description);
	sink->volume = 1.0f;
	sink->sent_pct = -1;
	sink->pending_pct = -1;
	sink->process_latency.ns = (uint64_t)latency_ms * SPA_NSEC_PER_MSEC;
	sink->timer = pw_loop_add_timer(data_loop, on_timeout, sink);

	node_name = make_node_name(key);
	props = pw_properties_new(
		PW_KEY_MEDIA_CLASS, "Audio/Sink",
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_NODE_NAME, node_name,
		PW_KEY_NODE_DESCRIPTION, description,
		PW_KEY_NODE_NICK, description,
		PW_KEY_MEDIA_NAME, description,
		PW_KEY_DEVICE_ICON_NAME, "audio-speakers",
		PW_KEY_NODE_NETWORK, "true",
		PW_KEY_NODE_LATENCY, "1024/48000",
		PW_KEY_PRIORITY_DRIVER, "500",
		/* Never grab the default output on our own, like other network sinks */
		PW_KEY_PRIORITY_SESSION, "100",
		"device.api", "airplay",
		/* The volume is the speaker's own, which we mirror. WirePlumber
		 * would otherwise restore the last one it saw for this name over
		 * it, and push that to the speaker. */
		"state.restore-props", "false",
		"state.restore-target", "false",
		"pw-airplay.target", key,
		NULL);
	free(node_name);

	sink->stream = pw_stream_new(core, description, props);
	if (!sink->stream) {
		pw_log_error("can't create stream for '%s': %m", description);
		sink_destroy(sink);
		return NULL;
	}
	pw_stream_add_listener(sink->stream, &sink->stream_listener, &stream_events, sink);

	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
	res = pw_stream_connect(sink->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
				PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
				PW_STREAM_FLAG_DRIVER,
				params, 1);
	if (res < 0) {
		pw_log_error("can't connect stream for '%s': %s", description, spa_strerror(res));
		sink_destroy(sink);
		return NULL;
	}
	return sink;
}

void sink_destroy(struct sink *sink)
{
	if (!sink)
		return;
	if (sink->stream) {
		spa_hook_remove(&sink->stream_listener);
		pw_loop_invoke(sink->data_loop, do_driver_stop, 0, NULL, 0, true, sink);
		pw_stream_destroy(sink->stream);
	}
	pw_loop_destroy_source(sink->data_loop, sink->timer);
	for (int i = 0; i < sink->n_ids; i++)
		free(sink->ids[i]);
	free(sink->ids);
	free(sink->key);
	free(sink->description);
	free(sink);
}

const char *sink_key(struct sink *sink)
{
	return sink->key;
}

const char *sink_description(struct sink *sink)
{
	return sink->description;
}

void sink_set_description(struct sink *sink, const char *description)
{
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(PW_KEY_NODE_DESCRIPTION, description),
		SPA_DICT_ITEM_INIT(PW_KEY_NODE_NICK, description),
		SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_NAME, description),
	};

	if (strcmp(sink->description, description) == 0)
		return;
	free(sink->description);
	sink->description = xstrdup(description);
	pw_stream_update_properties(sink->stream, &SPA_DICT_INIT_ARRAY(items));
}

void sink_set_outputs(struct sink *sink, char **ids, int n)
{
	for (int i = 0; i < sink->n_ids; i++)
		free(sink->ids[i]);
	free(sink->ids);
	sink->ids = xcalloc(n ? n : 1, sizeof(char *));
	for (int i = 0; i < n; i++)
		sink->ids[i] = xstrdup(ids[i]);
	sink->n_ids = n;
}

char **sink_outputs(struct sink *sink, int *n)
{
	*n = sink->n_ids;
	return sink->ids;
}

void sink_set_latency(struct sink *sink, uint32_t latency_ms)
{
	int64_t ns = (int64_t)latency_ms * SPA_NSEC_PER_MSEC;

	if (sink->process_latency.ns == ns)
		return;
	sink->process_latency.ns = ns;
	update_latency_params(sink);
}

void sink_set_device_volume(struct sink *sink, int pct)
{
	float vols[ENGINE_CHANNELS];

	if (pct == sink->sent_pct || pct < 0)
		return;
	if (sink->state != PW_STREAM_STATE_PAUSED && sink->state != PW_STREAM_STATE_STREAMING) {
		sink->pending_pct = pct;
		return;
	}

	/* Mark as already sent, so on_props_changed doesn't echo it back */
	sink->sent_pct = pct;
	for (int i = 0; i < ENGINE_CHANNELS; i++)
		vols[i] = pct_to_volume(pct);
	pw_stream_set_control(sink->stream, SPA_PROP_channelVolumes, ENGINE_CHANNELS, vols, 0);
}

int sink_volume_pct(struct sink *sink)
{
	return volume_to_pct(sink->volume, sink->mute);
}

bool sink_is_streaming(struct sink *sink)
{
	return sink->state == PW_STREAM_STATE_STREAMING;
}

uint64_t sink_last_audio_ns(struct sink *sink)
{
	return atomic_load(&sink->last_audio_ns);
}

uint32_t sink_node_id(struct sink *sink)
{
	return pw_stream_get_node_id(sink->stream);
}
