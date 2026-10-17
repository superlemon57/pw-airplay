/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - AirPlay 2 sender integrated with PipeWire
 *
 * Creates a PipeWire sink for every AirPlay 2 speaker (a HomePod stereo pair
 * counts as one), for every configured group, and for all speakers together.
 * Picking one of them as output in the desktop's sound settings is all it
 * takes: the speakers are connected when audio starts and released after
 * idle_timeout seconds of silence. The AirPlay protocol itself (HomeKit
 * pairing, encryption, PTP timing, multi-room sync) is done by a patched
 * OwnTone instance, which we feed through a pipe and control over its API.
 */
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <curl/curl.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "util.h"
#include "config.h"
#include "engine.h"
#include "mdns.h"
#include "nowplaying.h"
#include "owntone.h"
#include "sink.h"

#define POLL_INTERVAL_MS 2000
#define SUPERVISE_INTERVAL_MS 100
/* A sink whose speakers disappeared is kept this long, mDNS is flaky */
#define MISSING_GRACE_NS (60 * SPA_NSEC_PER_SEC)
/* Audio newer than this counts as "playing now" */
#define RECENT_AUDIO_NS (500 * SPA_NSEC_PER_MSEC)
/* How long OwnTone may take to start playing before we retry */
#define START_TIMEOUT_NS (10 * SPA_NSEC_PER_SEC)
/* Speakers dropping out before this are a failed start, not a takeover */
#define TAKEOVER_MIN_NS (15 * SPA_NSEC_PER_SEC)
#define RETRY_DELAY_NS (10 * SPA_NSEC_PER_SEC)

struct dsink {
	struct sink *sink;
	bool single;		/* one speaker, so its volume can be mirrored */
	uint64_t missing_since;
	struct dsink *next;
};

struct daemon {
	struct config cfg;
	struct pw_main_loop *main_loop;
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_loop *data_loop;

	struct engine *engine;
	struct ot_client *ot;
	struct mdns *mdns;
	struct nowplaying *np;

	struct spa_source *poll_timer;
	struct spa_source *supervise_timer;

	struct dsink *sinks;
	struct ot_output *outputs;
	int n_outputs;
	bool api_up;

	/* The sink currently playing on the speakers */
	struct sink *owner;
	uint64_t owner_started;
	bool owner_confirmed;	/* OwnTone reported playback */
	/* When OwnTone was first seen playing to our speakers */
	uint64_t selected_since;
	/* After a failed start, don't retry before this */
	uint64_t retry_after;
	/* Another sender took our speakers, don't fight it until we go idle */
	bool evicted;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

static uint32_t total_latency_ms(struct daemon *d)
{
	/* OwnTone schedules audio start_buffer_ms after reading it, and keeps
	 * pipe_start_buffer_ms in its input buffer (see the OwnTone patch).
	 * The rest is our own ring and the pipe. */
	int ms = d->cfg.start_buffer_ms + d->cfg.pipe_start_buffer_ms +
		 (int)engine_target_level_ms() + 10 + d->cfg.latency_offset_ms;
	return ms > 0 ? (uint32_t)ms : 0;
}

static struct ot_output *find_output(struct daemon *d, const char *id)
{
	for (int i = 0; i < d->n_outputs; i++)
		if (strcmp(d->outputs[i].id, id) == 0)
			return &d->outputs[i];
	return NULL;
}

static struct dsink *find_sink(struct daemon *d, const char *key)
{
	for (struct dsink *ds = d->sinks; ds; ds = ds->next)
		if (strcmp(sink_key(ds->sink), key) == 0)
			return ds;
	return NULL;
}

/* ------------------------------ Speakers --------------------------------- */

static void on_sink_volume(void *data, struct sink *sink, int pct)
{
	struct daemon *d = data;
	char **ids;
	int n;

	ids = sink_outputs(sink, &n);
	for (int i = 0; i < n; i++) {
		if (ot_set_volume(d->ot, ids[i], pct) < 0)
			pw_log_warn("could not set volume of output %s", ids[i]);
		else {
			struct ot_output *o = find_output(d, ids[i]);
			if (o)
				o->volume = pct;
		}
	}
	pw_log_info("'%s': volume %d%%", sink_description(sink), pct);
}

static const struct sink_events sink_events = {
	.volume_changed = on_sink_volume,
};

static void release_speakers(struct daemon *d, const char *why)
{
	if (!d->owner && !engine_running(d->engine))
		return;

	pw_log_info("releasing speakers (%s)", why);
	engine_stop(d->engine);
	/* Closing the pipe already makes OwnTone stop, this is for good measure */
	ot_player_stop(d->ot);
	ot_set_outputs(d->ot, NULL, 0);
	d->owner = NULL;
	if (d->np)
		nowplaying_set_owner(d->np, NULL);
	d->owner_confirmed = false;
	d->selected_since = 0;
}

static int claim_speakers(struct daemon *d, struct sink *sink)
{
	char **ids;
	int n, res;

	ids = sink_outputs(sink, &n);
	if (n == 0)
		return -ENODEV;

	/* Outputs must be selected before audio arrives, OwnTone starts
	 * playback (to the selected outputs) as soon as the pipe has data */
	if ((res = ot_set_outputs(d->ot, ids, n)) < 0) {
		pw_log_warn("could not select outputs for '%s'", sink_description(sink));
		return res;
	}
	for (int i = 0; i < n; i++)
		ot_set_volume(d->ot, ids[i], sink_volume_pct(sink));

	engine_set_owner(d->engine, sink);
	if (!engine_running(d->engine)) {
		if ((res = engine_start(d->engine)) < 0) {
			pw_log_warn("could not open %s: %s", d->cfg.fifo_path, spa_strerror(res));
			engine_set_owner(d->engine, NULL);
			return res;
		}
	}

	if (d->owner != sink) {
		char via[256] = "";

		for (int i = 0; i < n; i++) {
			struct ot_output *o = find_output(d, ids[i]);
			snprintf(via + strlen(via), sizeof(via) - strlen(via), "%s%s",
				 i ? ", " : "", o ? o->name : ids[i]);
		}
		pw_log_info("playing '%s' on AirPlay via %s (latency %u ms)", sink_description(sink),
			    via, total_latency_ms(d));
	}
	d->owner = sink;
	if (d->np)
		nowplaying_set_owner(d->np, sink);
	d->owner_started = now_ns();
	d->owner_confirmed = false;
	d->selected_since = 0;
	return 0;
}

/* ------------------------------- Targets --------------------------------- */

struct target {
	char key[160];
	char description[256];
	char **ids;
	int n_ids;
	bool single;
};

static void target_add_id(struct target *t, const char *id)
{
	t->ids = xrealloc(t->ids, sizeof(char *) * (t->n_ids + 1));
	t->ids[t->n_ids++] = (char *)id;
}

/* A speaker as the user sees it: one device, or a stereo pair. A pair's
 * leader doesn't relay a third-party sender's stream to its partner, so like
 * Apple senders we stream to both, each plays its own channel (in sync via
 * PTP). The leader comes first, its volume is the one mirrored. */
#define UNIT_MAX_MEMBERS 8
struct unit {
	char key[160];
	char description[256];
	const char *ids[UNIT_MAX_MEMBERS];
	int n_ids;
	bool computer;
};

static int build_units(struct daemon *d, struct unit **units, int *output_unit)
{
	struct unit *u = xcalloc(d->n_outputs ? d->n_outputs : 1, sizeof(*u));
	int n = 0;

	for (int i = 0; i < d->n_outputs; i++) {
		struct ot_output *o = &d->outputs[i];
		struct mdns_info info;
		const char *description;
		char key[160];
		int j;

		output_unit[i] = -1;
		if (strncmp(o->type, "AirPlay", 7) != 0)
			continue;

		/* Wait until we know whether it's part of a pair, otherwise both
		 * halves would briefly show up as separate speakers */
		if (!mdns_lookup(d->mdns, o->name, &info) && d->mdns)
			continue;
		description = info.group ? info.group : o->name;
		if (config_is_hidden(&d->cfg, o->name) || config_is_hidden(&d->cfg, description)) {
			mdns_info_clear(&info);
			continue;
		}

		/* The key must not depend on which device leads the pair, so the
		 * sink stays the same (and selected) when leadership changes */
		if (info.pair_id)
			snprintf(key, sizeof(key), "pair-%s", info.pair_id);
		else
			snprintf(key, sizeof(key), "%s", o->id);

		for (j = 0; j < n; j++)
			if (strcmp(u[j].key, key) == 0)
				break;
		if (j == n) {
			snprintf(u[j].key, sizeof(u[j].key), "%s", key);
			snprintf(u[j].description, sizeof(u[j].description), "%s", description);
			u[j].computer = info.computer;
			n++;
		}
		if (u[j].n_ids < UNIT_MAX_MEMBERS) {
			if (info.leader) {
				memmove(&u[j].ids[1], &u[j].ids[0], sizeof(char *) * u[j].n_ids);
				u[j].ids[0] = o->id;
			} else {
				u[j].ids[u[j].n_ids] = o->id;
			}
			u[j].n_ids++;
		}
		output_unit[i] = j;
		mdns_info_clear(&info);
	}
	*units = u;
	return n;
}

static int build_targets(struct daemon *d, struct target **targets)
{
	int *output_unit = xcalloc(d->n_outputs ? d->n_outputs : 1, sizeof(int));
	struct target *t;
	struct unit *u;
	int n_units, n_groups = 0, n = 0, n_all = 0;

	n_units = build_units(d, &u, output_unit);
	for (struct group_cfg *g = d->cfg.groups; g; g = g->next)
		n_groups++;
	t = xcalloc(n_units + n_groups + 1, sizeof(*t));

	for (int i = 0; i < n_units; i++) {
		snprintf(t[n].key, sizeof(t[n].key), "%s", u[i].key);
		snprintf(t[n].description, sizeof(t[n].description), "%s", u[i].description);
		t[n].single = true;
		for (int k = 0; k < u[i].n_ids; k++)
			target_add_id(&t[n], u[i].ids[k]);
		n++;
		/* Playing on someone's laptop is never what "all speakers" means */
		if (!u[i].computer)
			n_all++;
	}

	for (struct group_cfg *g = d->cfg.groups; g; g = g->next) {
		struct target grp = { 0 };
		bool used[n_units ? n_units : 1];

		memset(used, 0, sizeof(used));
		/* Members are speaker (pair) names, or a device's own name */
		for (int m = 0; m < g->n_members; m++) {
			int found = -1;

			for (int i = 0; i < n_units && found < 0; i++)
				if (strcasecmp(g->members[m], u[i].description) == 0)
					found = i;
			for (int i = 0; i < d->n_outputs && found < 0; i++)
				if (output_unit[i] >= 0 && strcasecmp(g->members[m], d->outputs[i].name) == 0)
					found = output_unit[i];
			if (found >= 0 && !used[found]) {
				used[found] = true;
				for (int k = 0; k < u[found].n_ids; k++)
					target_add_id(&grp, u[found].ids[k]);
			}
		}
		if (grp.n_ids == 0)
			continue;
		snprintf(grp.key, sizeof(grp.key), "group-%s", g->name);
		snprintf(grp.description, sizeof(grp.description), "%s", g->name);
		t[n++] = grp;
	}

	if (d->cfg.all_speakers_sink && n_all >= 2) {
		struct target *all = &t[n++];

		snprintf(all->key, sizeof(all->key), "all");
		snprintf(all->description, sizeof(all->description), "%s", d->cfg.all_speakers_name);
		for (int i = 0; i < n_units; i++)
			if (!u[i].computer)
				for (int k = 0; k < u[i].n_ids; k++)
					target_add_id(all, u[i].ids[k]);
	}

	free(u);
	free(output_unit);
	*targets = t;
	return n;
}

static bool same_outputs(struct sink *sink, char **ids, int n)
{
	int cur_n;
	char **cur = sink_outputs(sink, &cur_n);

	if (cur_n != n)
		return false;
	for (int i = 0; i < n; i++)
		if (strcmp(cur[i], ids[i]) != 0)
			return false;
	return true;
}

static void destroy_dsink(struct daemon *d, struct dsink *ds)
{
	struct dsink **p;

	if (d->owner == ds->sink)
		release_speakers(d, "speakers gone");
	for (p = &d->sinks; *p; p = &(*p)->next)
		if (*p == ds) {
			*p = ds->next;
			break;
		}
	pw_log_info("removing sink '%s'", sink_description(ds->sink));
	sink_destroy(ds->sink);
	free(ds);
}

static void reconcile_sinks(struct daemon *d)
{
	struct target *targets;
	struct dsink *ds, *next;
	uint64_t now = now_ns();
	int n;

	n = build_targets(d, &targets);

	for (int i = 0; i < n; i++) {
		struct target *t = &targets[i];

		ds = find_sink(d, t->key);
		if (!ds) {
			struct sink *s = sink_new(d->core, d->data_loop, d->engine, t->key,
						  t->description, total_latency_ms(d),
						  &sink_events, d);
			if (!s)
				continue;
			ds = xcalloc(1, sizeof(*ds));
			ds->sink = s;
			ds->single = t->single;
			ds->next = d->sinks;
			d->sinks = ds;
			pw_log_info("new sink '%s' (%d device%s)", t->description, t->n_ids,
				    t->n_ids > 1 ? "s" : "");
		}
		ds->missing_since = 0;
		sink_set_description(ds->sink, t->description);
		if (!same_outputs(ds->sink, t->ids, t->n_ids)) {
			sink_set_outputs(ds->sink, t->ids, t->n_ids);
			if (d->owner == ds->sink) {
				pw_log_info("'%s': speakers changed, switching", t->description);
				ot_set_outputs(d->ot, t->ids, t->n_ids);
			}
		}

		/* Mirror the speaker's own volume, which can change from its
		 * buttons, Siri or another sender */
		if (ds->single) {
			struct ot_output *o = find_output(d, t->ids[0]);
			if (o)
				sink_set_device_volume(ds->sink, o->volume);
		}
	}

	for (ds = d->sinks; ds; ds = next) {
		bool present = false;

		next = ds->next;
		for (int i = 0; i < n && !present; i++)
			present = strcmp(targets[i].key, sink_key(ds->sink)) == 0;
		if (present)
			continue;
		if (ds->missing_since == 0)
			ds->missing_since = now;
		else if (now - ds->missing_since > MISSING_GRACE_NS)
			destroy_dsink(d, ds);
	}

	for (int i = 0; i < n; i++)
		free(targets[i].ids);
	free(targets);
}

static void on_poll(void *data, uint64_t expirations)
{
	struct daemon *d = data;
	struct ot_output *outputs;
	int n;

	if (ot_get_outputs(d->ot, &outputs, &n) < 0) {
		if (d->api_up)
			pw_log_warn("AirPlay engine not reachable, waiting for it");
		d->api_up = false;
		return;
	}
	if (!d->api_up)
		pw_log_info("connected to AirPlay engine");
	d->api_up = true;

	ot_free_outputs(d->outputs, d->n_outputs);
	d->outputs = outputs;
	d->n_outputs = n;
	reconcile_sinks(d);

	if (d->owner) {
		char state[16];
		bool any_selected = false;
		char **ids;
		int n_ids;

		if (ot_get_player_state(d->ot, state, sizeof(state)) == 0 && strcmp(state, "play") == 0)
			d->owner_confirmed = true;

		ids = sink_outputs(d->owner, &n_ids);
		for (int i = 0; i < n_ids; i++) {
			struct ot_output *o = find_output(d, ids[i]);
			any_selected |= o && o->selected;
		}
		if (any_selected && d->owner_confirmed && d->selected_since == 0)
			d->selected_since = now_ns();

		/* OwnTone deselects speakers that fail to start, and those another
		 * sender (e.g. an iPhone) takes over. Once we have been playing
		 * for a while it can only be the latter, and like macOS we let it
		 * have them. */
		if (d->owner_confirmed && !any_selected) {
			if (d->selected_since && now_ns() - d->selected_since > TAKEOVER_MIN_NS) {
				pw_log_info("speakers taken over by another AirPlay sender");
				release_speakers(d, "taken over");
				d->evicted = true;
			} else {
				pw_log_warn("'%s' failed to start, retrying shortly (see engine log)",
					    sink_description(d->owner));
				release_speakers(d, "start failed");
				d->retry_after = now_ns() + RETRY_DELAY_NS;
			}
		}
	}
}

/* ------------------------------ Supervisor ------------------------------- */

static void on_supervise(void *data, uint64_t expirations)
{
	struct daemon *d = data;
	uint64_t now = now_ns(), idle_ns = (uint64_t)d->cfg.idle_timeout_s * SPA_NSEC_PER_SEC;
	struct sink *playing = NULL;
	uint64_t newest = 0;
	struct engine_stats stats;

	if (!d->api_up)
		return;

	/* The sink with the most recent audio */
	for (struct dsink *ds = d->sinks; ds; ds = ds->next) {
		uint64_t last = sink_last_audio_ns(ds->sink);

		if (sink_is_streaming(ds->sink) && last > newest) {
			newest = last;
			playing = ds->sink;
		}
	}
	if (playing && now - newest > RECENT_AUDIO_NS)
		playing = NULL;

	engine_get_stats(d->engine, &stats);
	if (stats.broken) {
		/* OwnTone restarted or closed the pipe, reopen it */
		pw_log_warn("AirPlay engine closed the pipe, restarting stream");
		engine_stop(d->engine);
		if (d->owner)
			claim_speakers(d, d->owner);
		return;
	}

	if (d->owner) {
		uint64_t last = sink_last_audio_ns(d->owner);

		/* Paused by the app: keep the speakers (shown as paused, and
		 * resumable from them) for longer */
		if (d->np && nowplaying_tick(d->np, now))
			idle_ns = (uint64_t)d->cfg.pause_timeout_s * SPA_NSEC_PER_SEC;

		if (now - last > idle_ns) {
			release_speakers(d, "idle");
			return;
		}
		/* Switching outputs: another sink plays while ours is quiet */
		if (playing && playing != d->owner && now - last > RECENT_AUDIO_NS) {
			claim_speakers(d, playing);
			return;
		}
		/* OwnTone never started playing, e.g. speaker unreachable */
		if (!d->owner_confirmed && now - d->owner_started > START_TIMEOUT_NS) {
			struct sink *s = d->owner;

			pw_log_warn("'%s' did not start, retrying", sink_description(s));
			release_speakers(d, "start timeout");
			claim_speakers(d, s);
		}
		return;
	}

	if (d->evicted) {
		/* Only claim again after we went quiet, i.e. on new playback */
		if (!playing)
			d->evicted = false;
		return;
	}

	if (playing && now >= d->retry_after)
		claim_speakers(d, playing);
}

/* -------------------------------- Main ----------------------------------- */

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct daemon *d = data;

	pw_log_error("PipeWire error id:%u seq:%d res:%d (%s): %s", id, seq, res,
		     spa_strerror(res), message);
	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(d->main_loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void on_signal(void *data, int signal_number)
{
	struct daemon *d = data;

	pw_main_loop_quit(d->main_loop);
}

static void start_timer(struct daemon *d, struct spa_source *timer, int ms)
{
	struct timespec value = { ms / 1000, (ms % 1000) * SPA_NSEC_PER_MSEC };

	pw_loop_update_timer(d->loop, timer, &value, &value, false);
}

static char *default_config_path(void)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	char buf[PATH_MAX];

	if (xdg && *xdg)
		snprintf(buf, sizeof(buf), "%s/pw-airplay/pw-airplay.conf", xdg);
	else
		snprintf(buf, sizeof(buf), "%s/.config/pw-airplay/pw-airplay.conf", home ? home : "");
	return xstrdup(buf);
}

/* The engine's state directories and the pipe it reads from */
static int prepare_data_dir(const struct config *cfg)
{
	static const char *subdirs[] = { "", "/pipes", "/cache", "/log", "/control" };
	const char *fifos[] = { cfg->fifo_path, cfg->metadata_fifo_path, cfg->remote_fifo_path };
	char path[PATH_MAX];
	struct stat st;

	for (size_t i = 0; i < SPA_N_ELEMENTS(subdirs); i++) {
		snprintf(path, sizeof(path), "%s%s", cfg->data_dir, subdirs[i]);
		if (mkdir(path, 0700) < 0 && errno != EEXIST)
			return -errno;
	}
	for (size_t i = 0; i < SPA_N_ELEMENTS(fifos); i++) {
		if (stat(fifos[i], &st) == 0 && !S_ISFIFO(st.st_mode))
			return -EEXIST;
		if (mkfifo(fifos[i], 0600) < 0 && errno != EEXIST)
			return -errno;
	}
	return 0;
}

static void usage(const char *name)
{
	printf("Usage: %s [options]\n"
	       "  -c, --config PATH          configuration file (default ~/.config/pw-airplay/pw-airplay.conf)\n"
	       "      --write-engine-conf P  write the AirPlay engine (OwnTone) config to P and exit\n"
	       "      --print-latency        print the latency reported to PipeWire and exit\n"
	       "  -h, --help\n", name);
}

int main(int argc, char *argv[])
{
	static const struct option long_options[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "write-engine-conf", required_argument, NULL, 'w' },
		{ "print-latency", no_argument, NULL, 'l' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct daemon d = { 0 };
	char *config_path = NULL, *engine_conf = NULL;
	bool print_latency = false;
	int c, res;

	while ((c = getopt_long(argc, argv, "c:h", long_options, NULL)) != -1) {
		switch (c) {
		case 'c':
			free(config_path);
			config_path = xstrdup(optarg);
			break;
		case 'w':
			engine_conf = optarg;
			break;
		case 'l':
			print_latency = true;
			break;
		case 'h':
			usage(argv[0]);
			free(config_path);
			return 0;
		default:
			usage(argv[0]);
			free(config_path);
			return 1;
		}
	}
	if (!config_path)
		config_path = default_config_path();

	if ((res = config_load(&d.cfg, config_path)) < 0) {
		fprintf(stderr, "can't read %s: %s\n", config_path, strerror(-res));
		return 1;
	}
	free(config_path);

	if (engine_conf) {
		if ((res = prepare_data_dir(&d.cfg)) < 0) {
			fprintf(stderr, "can't prepare %s: %s\n", d.cfg.data_dir, strerror(-res));
			return 1;
		}
		if ((res = config_write_owntone_conf(&d.cfg, engine_conf)) < 0) {
			fprintf(stderr, "can't write %s: %s\n", engine_conf, strerror(-res));
			return 1;
		}
		return 0;
	}
	if (print_latency) {
		printf("%u\n", total_latency_ms(&d));
		return 0;
	}

	signal(SIGPIPE, SIG_IGN);
	curl_global_init(CURL_GLOBAL_DEFAULT);
	pw_init(&argc, &argv);

	d.main_loop = pw_main_loop_new(NULL);
	d.loop = pw_main_loop_get_loop(d.main_loop);
	pw_loop_add_signal(d.loop, SIGINT, on_signal, &d);
	pw_loop_add_signal(d.loop, SIGTERM, on_signal, &d);

	/* One data loop, so the sinks' process callbacks, their driver timers
	 * and the pacer all run on the same realtime thread */
	d.context = pw_context_new(d.loop,
				   pw_properties_new("context.num-data-loops", "1",
						     PW_KEY_APP_NAME, "pw-airplay",
						     NULL),
				   0);
	if (!d.context) {
		fprintf(stderr, "can't create PipeWire context: %m\n");
		return 1;
	}
	d.data_loop = pw_context_acquire_loop(d.context, NULL);

	d.core = pw_context_connect(d.context, NULL, 0);
	if (!d.core) {
		fprintf(stderr, "can't connect to PipeWire: %m\n");
		return 1;
	}
	pw_core_add_listener(d.core, &d.core_listener, &core_events, &d);

	d.engine = engine_new(d.data_loop, d.cfg.fifo_path);
	d.ot = ot_client_new(d.cfg.api_port);
	d.mdns = mdns_new();
	if (d.cfg.now_playing)
		d.np = nowplaying_new(d.core, d.loop, d.ot, d.cfg.metadata_fifo_path, d.cfg.remote_fifo_path);

	d.poll_timer = pw_loop_add_timer(d.loop, on_poll, &d);
	d.supervise_timer = pw_loop_add_timer(d.loop, on_supervise, &d);
	start_timer(&d, d.poll_timer, POLL_INTERVAL_MS);
	start_timer(&d, d.supervise_timer, SUPERVISE_INTERVAL_MS);
	on_poll(&d, 1);

	pw_log_info("pw-airplay running, reported latency %u ms", total_latency_ms(&d));
	pw_main_loop_run(d.main_loop);

	release_speakers(&d, "shutting down");
	while (d.sinks)
		destroy_dsink(&d, d.sinks);
	pw_loop_destroy_source(d.loop, d.poll_timer);
	pw_loop_destroy_source(d.loop, d.supervise_timer);
	engine_free(d.engine);
	ot_free_outputs(d.outputs, d.n_outputs);
	ot_client_free(d.ot);
	mdns_free(d.mdns);
	nowplaying_free(d.np);
	pw_core_disconnect(d.core);
	pw_context_release_loop(d.context, d.data_loop);
	pw_context_destroy(d.context);
	pw_main_loop_destroy(d.main_loop);
	config_free(&d.cfg);
	curl_global_cleanup();
	pw_deinit();
	return 0;
}
