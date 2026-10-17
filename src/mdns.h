/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - speaker grouping from mDNS */
#pragma once

#include <stdbool.h>

struct mdns;

struct mdns *mdns_new(void);
void mdns_free(struct mdns *m);

struct mdns_info {
	/* Name of the stereo pair / Home group the device belongs to ("gpn") */
	char *group;
	/* Devices playing as one (a stereo pair) share this "tsid" */
	char *pair_id;
	/* Currently leads its pair ("igl"), its volume stands for the pair's */
	bool leader;
	/* A Mac or other computer receiving AirPlay, not a speaker */
	bool computer;
};

/* Returns false if the device hasn't been resolved (yet). Strings in info
 * must be freed with mdns_info_clear(). */
bool mdns_lookup(struct mdns *m, const char *service_name, struct mdns_info *info);
void mdns_info_clear(struct mdns_info *info);
