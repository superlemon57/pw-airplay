/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - a PipeWire Audio/Sink for one AirPlay target (device or group) */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pipewire/pipewire.h>

#include "engine.h"

struct sink;

struct sink_events {
	/* The user changed the sink volume, pct is 0-100 (0 when muted) */
	void (*volume_changed)(void *data, struct sink *sink, int pct);
};

struct sink *sink_new(struct pw_core *core, struct pw_loop *data_loop, struct engine *engine,
		      const char *key, const char *description, uint32_t latency_ms,
		      const struct sink_events *events, void *data);
void sink_destroy(struct sink *sink);

const char *sink_key(struct sink *sink);
const char *sink_description(struct sink *sink);
void sink_set_description(struct sink *sink, const char *description);

/* The OwnTone output ids this sink plays to */
void sink_set_outputs(struct sink *sink, char **ids, int n);
char **sink_outputs(struct sink *sink, int *n);

void sink_set_latency(struct sink *sink, uint32_t latency_ms);
/* Reflects the speaker's volume (changed elsewhere) in the sink's volume */
void sink_set_device_volume(struct sink *sink, int pct);
int sink_volume_pct(struct sink *sink);

/* PipeWire node id of the sink, SPA_ID_INVALID until connected */
uint32_t sink_node_id(struct sink *sink);

/* An application is connected and the graph is running */
bool sink_is_streaming(struct sink *sink);
/* Monotonic time of the last non-silent audio, 0 if never */
uint64_t sink_last_audio_ns(struct sink *sink);
