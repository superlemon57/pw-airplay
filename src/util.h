/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - allocation helpers: out of memory is fatal, systemd restarts us */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void *oom(void *p)
{
	if (!p) {
		fputs("pw-airplayd: out of memory\n", stderr);
		abort();
	}
	return p;
}

#define xmalloc(size) oom(malloc(size))
#define xcalloc(n, size) oom(calloc(n, size))
#define xrealloc(p, size) oom(realloc(p, size))
#define xstrdup(s) ((char *)oom(strdup(s)))

/* Time since then. Timestamps taken on another thread can be slightly newer
 * than our "now", which must not wrap around to a huge interval. */
static inline uint64_t elapsed_ns(uint64_t now, uint64_t then)
{
	return now > then ? now - then : 0;
}
