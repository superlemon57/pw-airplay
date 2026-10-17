/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - allocation helpers: out of memory is fatal, systemd restarts us */
#pragma once

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
