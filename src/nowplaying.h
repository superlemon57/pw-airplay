/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - "Now Playing" on the speakers and remote control from them */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pipewire/pipewire.h>

#include "owntone.h"
#include "sink.h"

struct nowplaying;

struct nowplaying *nowplaying_new(struct pw_core *core, struct pw_loop *loop, struct ot_client *ot,
				  const char *metadata_fifo, const char *remote_fifo);
void nowplaying_free(struct nowplaying *np);

/* The sink currently playing on the speakers, or NULL once released */
void nowplaying_set_owner(struct nowplaying *np, struct sink *owner);

/* Called periodically: keeps the AirPlay play/pause state in line with the
 * application's. Returns true while playback is paused by the application. */
bool nowplaying_tick(struct nowplaying *np, uint64_t now_ns);
