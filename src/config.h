/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - AirPlay 2 sender integrated with PipeWire */
#pragma once

#include <stdbool.h>

struct group_cfg {
	char *name;
	char **members;
	int n_members;
	struct group_cfg *next;
};

struct config {
	/* Seconds without audio before the speakers are released */
	int idle_timeout_s;
	/* Same, while the application says it is paused: the speakers stay
	 * "paused" (and resumable from an iPhone) this long */
	int pause_timeout_s;
	/* Show what's playing on the speakers, take commands from them */
	bool now_playing;
	/* OwnTone engine buffering, see owntone.conf */
	int start_buffer_ms;
	int pipe_start_buffer_ms;
	/* Added to the reported latency, to calibrate A/V sync by ear */
	int latency_offset_ms;
	int api_port;
	/* How this computer shows up on the speakers and the iPhone */
	char *sender_name;
	/* OwnTone's log level: "log" (default), "info" or "debug" */
	char *engine_log_level;
	bool all_speakers_sink;
	char *all_speakers_name;
	char **hidden;
	int n_hidden;
	struct group_cfg *groups;

	char *config_path;
	char *data_dir;
	char *fifo_path;
	char *metadata_fifo_path;
	char *remote_fifo_path;
};

int config_load(struct config *cfg, const char *path);
void config_free(struct config *cfg);
bool config_is_hidden(const struct config *cfg, const char *name);
int config_write_owntone_conf(const struct config *cfg, const char *path);
