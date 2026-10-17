/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - client for the OwnTone JSON API */
#pragma once

#include <stdbool.h>

struct ot_output {
	char *id;
	char *name;
	char *type;
	bool selected;
	int volume;
};

struct ot_client;

struct ot_client *ot_client_new(int port);
void ot_client_free(struct ot_client *ot);

/* All return 0 on success, negative on error */
int ot_get_outputs(struct ot_client *ot, struct ot_output **outputs, int *n);
void ot_free_outputs(struct ot_output *outputs, int n);
int ot_set_outputs(struct ot_client *ot, char *const *ids, int n);
int ot_set_volume(struct ot_client *ot, const char *id, int volume);
/* state is one of "play", "pause", "stop" */
int ot_get_player_state(struct ot_client *ot, char *state, int len);
int ot_player_stop(struct ot_client *ot);
int ot_player_pause(struct ot_client *ot);
int ot_player_play(struct ot_client *ot);
