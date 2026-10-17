/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - MPRIS client: what desktop media players are playing */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pipewire/pipewire.h>

struct mpris_player {
	char *bus_name;		/* org.mpris.MediaPlayer2.<name> */
	char *unique_name;
	uint32_t pid;
	char *identity;
	char *desktop_entry;
	char *status;		/* "Playing", "Paused" or "Stopped" */

	char *track_id;
	char *title;
	char *artist;
	char *album;
	char *art_url;
	int64_t length_us;	/* 0 if unknown */

	/* Bumped on every Seeked signal */
	uint32_t seek_serial;

	struct mpris_player *next;
};

struct mpris_events {
	/* Something changed: players, their state or metadata */
	void (*changed)(void *data);
};

struct mpris;

struct mpris *mpris_new(struct pw_loop *loop, const struct mpris_events *events, void *data);
void mpris_free(struct mpris *m);

/* Valid until the next return to the main loop */
struct mpris_player *mpris_players(struct mpris *m);
struct mpris_player *mpris_find(struct mpris *m, const char *bus_name);

/* Position is not signalled by MPRIS, it has to be asked for */
int mpris_position(struct mpris *m, struct mpris_player *p, int64_t *pos_us);
/* Calls a method of org.mpris.MediaPlayer2.Player, e.g. "PlayPause" */
void mpris_command(struct mpris *m, struct mpris_player *p, const char *method);
