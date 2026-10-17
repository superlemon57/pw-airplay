/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - "Now Playing" on the speakers and remote control from them
 *
 * Like macOS: the speakers (and an iPhone looking at them) show what is
 * playing, and their play/pause/next/previous controls act on the application.
 *
 * - Which application: the MPRIS player whose process feeds the owner sink,
 *   found by following PipeWire links from the sink to the stream nodes and
 *   matching their process (or, failing that, application name).
 * - Metadata goes to OwnTone's metadata pipe, in the shairport-sync format it
 *   reads, from a writer thread (artwork may need fetching, writes may block).
 * - Pause: when the app pauses we pause AirPlay too, so the speakers show it as
 *   paused instead of playing silence; resuming goes through OwnTone's live
 *   trim, so latency stays the same.
 * - Commands: OwnTone (patched) writes remote commands to a fifo instead of
 *   acting on them while our pipe plays; we pass them on over MPRIS.
 */
#include <ctype.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <spa/utils/dict.h>

#include "util.h"
#include "mpris.h"
#include "nowplaying.h"

/* OwnTone refuses bigger pictures */
#define ARTWORK_MAX (1024 * 1024)
#define WRITE_TIMEOUT_MS 2000
/* App reports paused and no audio for this long: pause AirPlay */
#define PAUSE_DELAY_NS (700 * SPA_NSEC_PER_MSEC)
#define RECENT_AUDIO_NS (300 * SPA_NSEC_PER_MSEC)
/* Progress, as OwnTone expects it, is in samples from an arbitrary non-zero base */
#define PROGRESS_RATE 44100
#define PROGRESS_BASE 1000000

struct stream_node {
	uint32_t id;
	uint32_t pid;
	char *app_name;
	char *binary;
	char *app_id;
	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook node_listener;
	struct nowplaying *np;
	struct stream_node *next;
};

struct link_info {
	uint32_t id;
	uint32_t output_node;
	uint32_t input_node;
	struct link_info *next;
};

enum job_kind {
	JOB_ITEM,	/* write data */
	JOB_TRACK,	/* write data, then the artwork at art_url, then suffix */
	JOB_OPEN,	/* try to open the pipe */
	JOB_CLOSE,	/* drop the pipe */
};

struct job {
	enum job_kind kind;
	char *data;
	size_t len;
	char *art_url;
	char *suffix;
	size_t suffix_len;
	struct job *next;
};

struct nowplaying {
	struct pw_loop *loop;
	struct ot_client *ot;
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	struct stream_node *nodes;
	struct link_info *links;

	struct mpris *mpris;
	char *player;		/* bus name of the app we show */

	struct sink *owner;
	bool ot_paused;
	uint64_t paused_since;

	/* What we last told the speakers */
	char *sent_key;
	char *sent_status;
	uint32_t sent_seek_serial;

	/* Remote commands from OwnTone */
	int remote_fd;
	struct spa_source *remote_io;
	char remote_buf[256];
	size_t remote_len;

	/* Writer thread */
	char *metadata_path;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct job *jobs;
	bool quit;
	int metadata_fd;	/* writer thread only */
	/* OwnTone (re)opened the metadata pipe: everything must be sent again */
	_Atomic bool resync;
	_Atomic bool connected;
	uint64_t last_probe;
	char *art_cache_url;	/* writer thread only */
	char *art_cache_item;
	size_t art_cache_len;
};

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64(const uint8_t *in, size_t len, size_t *out_len)
{
	char *out = xmalloc((len + 2) / 3 * 4 + 1), *p = out;

	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = in[i] << 16 | (i + 1 < len ? in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);

		*p++ = b64chars[(v >> 18) & 63];
		*p++ = b64chars[(v >> 12) & 63];
		*p++ = i + 1 < len ? b64chars[(v >> 6) & 63] : '=';
		*p++ = i + 2 < len ? b64chars[v & 63] : '=';
	}
	*p = '\0';
	*out_len = p - out;
	return out;
}

/* One item of shairport-sync's metadata pipe format, which OwnTone parses */
static char *make_item(const char *type, const char *code, const void *data, size_t len, size_t *out_len)
{
	size_t b64_len;
	char *b64 = base64(data, len, &b64_len);
	size_t size = b64_len + 160;
	char *item = xmalloc(size);
	uint32_t t = (uint32_t)type[0] << 24 | type[1] << 16 | type[2] << 8 | type[3];
	uint32_t c = (uint32_t)code[0] << 24 | code[1] << 16 | code[2] << 8 | code[3];

	*out_len = snprintf(item, size,
			    "<item><type>%08x</type><code>%08x</code><length>%zu</length>\n"
			    "<data encoding=\"base64\">\n%s</data></item>\n",
			    t, c, len, b64);
	free(b64);
	return item;
}

/* ----------------------------- Writer thread ------------------------------ */

static void writer_close(struct nowplaying *np)
{
	if (np->metadata_fd >= 0)
		close(np->metadata_fd);
	np->metadata_fd = -1;
	atomic_store(&np->connected, false);
}

/* OwnTone only has the pipe open while playing ours. Whatever was written
 * before is lost, so the main thread resends everything once we're in. */
static bool writer_open(struct nowplaying *np)
{
	if (np->metadata_fd >= 0)
		return true;
	np->metadata_fd = open(np->metadata_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (np->metadata_fd < 0)
		return false;
	atomic_store(&np->connected, true);
	atomic_store(&np->resync, true);
	return true;
}

static bool writer_write(struct nowplaying *np, const char *data, size_t len)
{
	if (!writer_open(np))
		return false;
	while (len > 0) {
		struct pollfd pfd = { .fd = np->metadata_fd, .events = POLLOUT };
		ssize_t res;

		if (poll(&pfd, 1, WRITE_TIMEOUT_MS) <= 0 || (pfd.revents & (POLLERR | POLLHUP))) {
			writer_close(np);
			return false;
		}
		res = write(np->metadata_fd, data, len);
		if (res < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			writer_close(np);
			return false;
		}
		data += res;
		len -= res;
	}
	return true;
}

struct fetch {
	uint8_t *data;
	size_t len;
};

static size_t on_fetch(char *data, size_t size, size_t nmemb, void *user)
{
	struct fetch *f = user;
	size_t len = size * nmemb;
	uint8_t *p;

	if (f->len + len > ARTWORK_MAX)
		return 0;
	if (!(p = realloc(f->data, f->len + len)))
		return 0;
	memcpy(p + f->len, data, len);
	f->data = p;
	f->len += len;
	return len;
}

/* Artwork comes as file:// (most players) or http(s):// (e.g. Spotify) */
static bool fetch_artwork(const char *url, struct fetch *f)
{
	memset(f, 0, sizeof(*f));

	if (strncmp(url, "file://", 7) == 0) {
		char *path = curl_unescape(url + 7, 0);
		FILE *file = path ? fopen(path, "rb") : NULL;
		uint8_t buf[16384];
		size_t n;
		bool ok = file != NULL;

		while (file && (n = fread(buf, 1, sizeof(buf), file)) > 0)
			if (on_fetch((char *)buf, 1, n, f) != n) {
				ok = false;
				break;
			}
		if (file)
			fclose(file);
		curl_free(path);
		return ok && f->len > 0;
	}

	if (strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0) {
		CURL *curl = curl_easy_init();
		long status = 0;
		CURLcode res;

		if (!curl)
			return false;
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_fetch);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if CURL_AT_LEAST_VERSION(7, 85, 0)
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#endif
		res = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		curl_easy_cleanup(curl);
		return res == CURLE_OK && status == 200 && f->len > 0;
	}
	return false;
}

static bool is_image(const uint8_t *d, size_t len)
{
	return (len > 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) ||	/* JPEG */
	       (len > 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0);
}

/* Returns the PICT item for url, fetching it unless it's the last one */
static const char *writer_artwork(struct nowplaying *np, const char *url, size_t *len)
{
	struct fetch f;

	*len = 0;
	if (!url)
		return NULL;

	if (!np->art_cache_url || strcmp(np->art_cache_url, url) != 0) {
		free(np->art_cache_url);
		free(np->art_cache_item);
		np->art_cache_url = xstrdup(url);
		np->art_cache_item = NULL;
		np->art_cache_len = 0;

		if (fetch_artwork(url, &f) && is_image(f.data, f.len))
			np->art_cache_item = make_item("ssnc", "PICT", f.data, f.len, &np->art_cache_len);
		else
			pw_log_info("no usable artwork at %s", url);
		free(f.data);
	}
	*len = np->art_cache_len;
	return np->art_cache_item;
}

/* OwnTone pushes metadata to the speakers after every read of the pipe, so
 * a track change goes out in a single write */
static void writer_track(struct nowplaying *np, struct job *job)
{
	size_t art_len, len;
	const char *art = writer_artwork(np, job->art_url, &art_len);
	char *buf;

	len = job->len + art_len + job->suffix_len;
	if (!(buf = malloc(len)))
		return;
	memcpy(buf, job->data, job->len);
	if (art)
		memcpy(buf + job->len, art, art_len);
	memcpy(buf + job->len + art_len, job->suffix, job->suffix_len);
	writer_write(np, buf, len);
	free(buf);
}

static void *writer_thread(void *arg)
{
	struct nowplaying *np = arg;

	pthread_mutex_lock(&np->lock);
	while (!np->quit) {
		struct job *job = np->jobs;

		if (!job) {
			pthread_cond_wait(&np->cond, &np->lock);
			continue;
		}
		np->jobs = job->next;
		pthread_mutex_unlock(&np->lock);

		switch (job->kind) {
		case JOB_ITEM:
			writer_write(np, job->data, job->len);
			break;
		case JOB_TRACK:
			writer_track(np, job);
			break;
		case JOB_OPEN:
			writer_open(np);
			break;
		case JOB_CLOSE:
			writer_close(np);
			break;
		}
		free(job->data);
		free(job->art_url);
		free(job->suffix);
		free(job);

		pthread_mutex_lock(&np->lock);
	}
	pthread_mutex_unlock(&np->lock);
	writer_close(np);
	return NULL;
}

static void queue(struct nowplaying *np, struct job *job)
{
	struct job **tail;

	pthread_mutex_lock(&np->lock);
	for (tail = &np->jobs; *tail; tail = &(*tail)->next)
		;
	*tail = job;
	pthread_cond_signal(&np->cond);
	pthread_mutex_unlock(&np->lock);
}

static void queue_simple(struct nowplaying *np, enum job_kind kind)
{
	struct job *job = xcalloc(1, sizeof(*job));

	job->kind = kind;
	queue(np, job);
}

/* Appends a text item to buf */
static void append_text(char **buf, size_t *len, const char *code, const char *value)
{
	size_t item_len;
	char *item = make_item("core", code, value ? value : "", value ? strlen(value) : 0, &item_len);

	*buf = xrealloc(*buf, *len + item_len);
	memcpy(*buf + *len, item, item_len);
	*len += item_len;
	free(item);
}

/* ------------------------------ PipeWire graph ---------------------------- */

static void node_free(struct stream_node *n)
{
	if (n->proxy) {
		spa_hook_remove(&n->node_listener);
		spa_hook_remove(&n->proxy_listener);
		pw_proxy_destroy(n->proxy);
	}
	free(n->app_name);
	free(n->binary);
	free(n->app_id);
	free(n);
}

static void on_node_info(void *data, const struct pw_node_info *info)
{
	struct stream_node *n = data;
	const char *s;

	if (!info->props)
		return;
	if ((s = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_ID)))
		n->pid = (uint32_t)strtoul(s, NULL, 10);
	if ((s = spa_dict_lookup(info->props, PW_KEY_APP_NAME))) {
		free(n->app_name);
		n->app_name = xstrdup(s);
	}
	if ((s = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY))) {
		free(n->binary);
		n->binary = xstrdup(s);
	}
	/* Flatpak app id, or the app's own id */
	if ((s = spa_dict_lookup(info->props, "pipewire.access.portal.app_id")) ||
	    (s = spa_dict_lookup(info->props, PW_KEY_APP_ID))) {
		free(n->app_id);
		n->app_id = xstrdup(s);
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info,
};

static void on_proxy_removed(void *data)
{
	struct stream_node *n = data;

	pw_proxy_destroy(n->proxy);
}

static void on_proxy_destroy(void *data)
{
	struct stream_node *n = data;

	spa_hook_remove(&n->node_listener);
	spa_hook_remove(&n->proxy_listener);
	n->proxy = NULL;
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = on_proxy_removed,
	.destroy = on_proxy_destroy,
};

static void on_global(void *data, uint32_t id, uint32_t permissions, const char *type,
		      uint32_t version, const struct spa_dict *props)
{
	struct nowplaying *np = data;
	const char *s;

	if (!props)
		return;

	if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
		struct link_info *l = xcalloc(1, sizeof(*l));
		const char *out = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
		const char *in = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);

		l->id = id;
		l->output_node = out ? (uint32_t)atoi(out) : SPA_ID_INVALID;
		l->input_node = in ? (uint32_t)atoi(in) : SPA_ID_INVALID;
		l->next = np->links;
		np->links = l;
		return;
	}

	if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0 &&
	    (s = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) && strcmp(s, "Stream/Output/Audio") == 0) {
		/* The registry only has a subset of the properties, the process
		 * id needs the full node info */
		struct stream_node *n = xcalloc(1, sizeof(*n));

		n->id = id;
		n->np = np;
		n->proxy = pw_registry_bind(np->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
		if (n->proxy) {
			pw_proxy_add_listener(n->proxy, &n->proxy_listener, &proxy_events, n);
			pw_proxy_add_object_listener(n->proxy, &n->node_listener, &node_events, n);
		}
		n->next = np->nodes;
		np->nodes = n;
	}
}

static void on_global_remove(void *data, uint32_t id)
{
	struct nowplaying *np = data;

	for (struct link_info **l = &np->links; *l; l = &(*l)->next)
		if ((*l)->id == id) {
			struct link_info *tmp = *l;
			*l = tmp->next;
			free(tmp);
			return;
		}
	for (struct stream_node **n = &np->nodes; *n; n = &(*n)->next)
		if ((*n)->id == id) {
			struct stream_node *tmp = *n;
			*n = tmp->next;
			node_free(tmp);
			return;
		}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
	.global_remove = on_global_remove,
};

/* ----------------------------- Player matching ---------------------------- */

static uint32_t parent_pid(uint32_t pid)
{
	char path[64], buf[512], *p;
	uint32_t ppid = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%u/stat", pid);
	if (!(f = fopen(path, "r")))
		return 0;
	if (fgets(buf, sizeof(buf), f) && (p = strrchr(buf, ')')))
		sscanf(p + 2, "%*c %u", &ppid);
	fclose(f);
	return ppid;
}

/* Browsers play audio from a child process of the one owning the bus name */
static bool same_process_tree(uint32_t a, uint32_t b)
{
	uint32_t p;
	int i;

	if (!a || !b)
		return false;
	for (p = a, i = 0; p > 1 && i < 8; p = parent_pid(p), i++)
		if (p == b)
			return true;
	for (p = b, i = 0; p > 1 && i < 8; p = parent_pid(p), i++)
		if (p == a)
			return true;
	return false;
}

/* Lowercase alphanumerics only, "org.mozilla.firefox" -> "orgmozillafirefox" */
static void normalize(char *out, size_t size, const char *in)
{
	size_t j = 0;

	for (; in && *in && j + 1 < size; in++)
		if (isalnum((unsigned char)*in))
			out[j++] = tolower((unsigned char)*in);
	out[j] = '\0';
}

static bool names_match(const char *a, const char *b)
{
	char na[128], nb[128];

	normalize(na, sizeof(na), a);
	normalize(nb, sizeof(nb), b);
	if (strlen(na) < 3 || strlen(nb) < 3)
		return false;
	return strstr(na, nb) || strstr(nb, na);
}

static bool player_matches_name(const struct mpris_player *p, const struct stream_node *n)
{
	const char *pnames[4] = { p->identity, p->desktop_entry, NULL, NULL };
	const char *snames[3] = { n->app_name, n->binary, n->app_id };
	char suffix[128];
	const char *base;

	/* org.mpris.MediaPlayer2.firefox.instance_1_23 -> firefox */
	snprintf(suffix, sizeof(suffix), "%s", p->bus_name + strlen("org.mpris.MediaPlayer2."));
	if (strchr(suffix, '.'))
		*strchr(suffix, '.') = '\0';
	pnames[2] = suffix;
	/* com.spotify.Client -> Client is useless, but the whole id is compared */
	base = p->desktop_entry ? strrchr(p->desktop_entry, '.') : NULL;
	pnames[3] = base ? base + 1 : NULL;

	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 3; j++)
			if (pnames[i] && snames[j] && names_match(pnames[i], snames[j]))
				return true;
	return false;
}

static int player_score(struct nowplaying *np, const struct mpris_player *p, uint32_t sink_node)
{
	int best = 0;

	for (struct link_info *l = np->links; l; l = l->next) {
		if (l->input_node != sink_node)
			continue;
		for (struct stream_node *n = np->nodes; n; n = n->next) {
			if (n->id != l->output_node)
				continue;
			if (same_process_tree(n->pid, p->pid))
				best = SPA_MAX(best, 3);
			else if (player_matches_name(p, n))
				best = SPA_MAX(best, 2);
		}
	}
	/* Among equals, the one actually playing */
	return best * 2 + (p->status && strcmp(p->status, "Playing") == 0);
}

static struct mpris_player *select_player(struct nowplaying *np)
{
	struct mpris_player *best = NULL;
	uint32_t sink_node;
	int best_score = 0;

	if (!np->owner)
		return NULL;
	sink_node = sink_node_id(np->owner);

	for (struct mpris_player *p = mpris_players(np->mpris); p; p = p->next) {
		int score = player_score(np, p, sink_node);

		/* A playing app not linked to our sink isn't ours: score 1 needs
		 * no other candidate, and then only as a last resort */
		if (score > best_score) {
			best = p;
			best_score = score;
		}
	}
	/* Score 1 means "playing, but no link to our sink found"; accept it
	 * only if it's the only player playing */
	if (best && best_score == 1) {
		int playing = 0;

		for (struct mpris_player *p = mpris_players(np->mpris); p; p = p->next)
			playing += p->status && strcmp(p->status, "Playing") == 0;
		if (playing != 1)
			best = NULL;
	}
	return best;
}

/* -------------------------------- Updates --------------------------------- */

static void progress_item(struct nowplaying *np, struct mpris_player *p, char **buf, size_t *len)
{
	int64_t pos_us = 0;
	char value[96];
	uint64_t pos, end;

	if (mpris_position(np->mpris, p, &pos_us) < 0 || pos_us < 0)
		pos_us = 0;
	pos = PROGRESS_BASE + (uint64_t)pos_us * PROGRESS_RATE / SPA_USEC_PER_SEC;
	end = p->length_us > 0 ? PROGRESS_BASE + (uint64_t)p->length_us * PROGRESS_RATE / SPA_USEC_PER_SEC
			       : PROGRESS_BASE;
	snprintf(value, sizeof(value), "%u/%" PRIu64 "/%" PRIu64, PROGRESS_BASE, pos, end);
	append_text(buf, len, "prgr", value);
}

static void send_progress(struct nowplaying *np, struct mpris_player *p)
{
	struct job *job = xcalloc(1, sizeof(*job));

	job->kind = JOB_ITEM;
	progress_item(np, p, &job->data, &job->len);
	queue(np, job);
}

static void send_track(struct nowplaying *np, struct mpris_player *p)
{
	struct job *job = xcalloc(1, sizeof(*job));

	job->kind = JOB_TRACK;
	append_text(&job->data, &job->len, "minm", p->title);
	append_text(&job->data, &job->len, "asar", p->artist);
	append_text(&job->data, &job->len, "asal", p->album);
	job->art_url = p->art_url ? xstrdup(p->art_url) : NULL;
	progress_item(np, p, &job->suffix, &job->suffix_len);
	queue(np, job);
}

static void update(struct nowplaying *np)
{
	struct mpris_player *p;
	char key[2048];

	if (!np->owner)
		return;

	p = select_player(np);
	/* Players may close their stream while paused, keep following the one
	 * we had until another one plays to the sink */
	if (!p)
		p = mpris_find(np->mpris, np->player);
	if (!p) {
		if (np->player)
			pw_log_info("now playing: no media player found for this output");
		free(np->player);
		np->player = NULL;
		return;
	}
	if (!np->player || strcmp(np->player, p->bus_name) != 0) {
		pw_log_info("now playing: following %s (%s)", p->bus_name, p->identity ? p->identity : "?");
		free(np->player);
		np->player = xstrdup(p->bus_name);
		free(np->sent_key);
		np->sent_key = NULL;
	}

	snprintf(key, sizeof(key), "%s\x1f%s\x1f%s\x1f%s\x1f%s\x1f%" PRIi64,
		 p->track_id ? p->track_id : "", p->title ? p->title : "", p->artist ? p->artist : "",
		 p->album ? p->album : "", p->art_url ? p->art_url : "", p->length_us);

	if (!np->sent_key || strcmp(np->sent_key, key) != 0) {
		pw_log_info("now playing: %s - %s", p->artist ? p->artist : "?", p->title ? p->title : "?");
		send_track(np, p);
		free(np->sent_key);
		np->sent_key = xstrdup(key);
		np->sent_seek_serial = p->seek_serial;
	} else if (p->seek_serial != np->sent_seek_serial) {
		send_progress(np, p);
		np->sent_seek_serial = p->seek_serial;
	}

	/* Position jumps on resume from the speakers' point of view */
	if (p->status && (!np->sent_status || strcmp(np->sent_status, p->status) != 0)) {
		if (strcmp(p->status, "Playing") == 0 && np->sent_status)
			send_progress(np, p);
		free(np->sent_status);
		np->sent_status = xstrdup(p->status);
	}
}

static void on_mpris_changed(void *data)
{
	update(data);
}

static const struct mpris_events mpris_events = {
	.changed = on_mpris_changed,
};

/* ----------------------------- Remote commands ---------------------------- */

static void handle_command(struct nowplaying *np, const char *cmd)
{
	static const struct { const char *cmd, *method; } map[] = {
		{ "playpause", "PlayPause" },
		{ "play", "Play" },
		{ "pause", "Pause" },
		{ "stop", "Pause" },
		{ "next", "Next" },
		{ "prev", "Previous" },
	};
	struct mpris_player *p = mpris_find(np->mpris, np->player);

	if (!p)
		p = select_player(np);
	for (size_t i = 0; i < SPA_N_ELEMENTS(map); i++) {
		if (strcmp(cmd, map[i].cmd) != 0)
			continue;
		if (!p) {
			pw_log_info("remote '%s': no media player to control", cmd);
			return;
		}
		pw_log_info("remote '%s' from speakers", cmd);
		mpris_command(np->mpris, p, map[i].method);
		return;
	}
	pw_log_warn("unknown remote command '%s'", cmd);
}

static void on_remote_io(void *data, int fd, uint32_t mask)
{
	struct nowplaying *np = data;
	ssize_t res;
	char *nl;

	res = read(fd, np->remote_buf + np->remote_len, sizeof(np->remote_buf) - np->remote_len - 1);
	if (res <= 0)
		return;
	np->remote_len += res;
	np->remote_buf[np->remote_len] = '\0';

	while ((nl = strchr(np->remote_buf, '\n'))) {
		*nl = '\0';
		if (*np->remote_buf)
			handle_command(np, np->remote_buf);
		np->remote_len -= nl + 1 - np->remote_buf;
		memmove(np->remote_buf, nl + 1, np->remote_len + 1);
	}
	/* A line that long is garbage */
	if (np->remote_len >= sizeof(np->remote_buf) - 1)
		np->remote_len = 0;
}

/* --------------------------------- API ------------------------------------ */

struct nowplaying *nowplaying_new(struct pw_core *core, struct pw_loop *loop, struct ot_client *ot,
				  const char *metadata_fifo, const char *remote_fifo)
{
	struct nowplaying *np = xcalloc(1, sizeof(*np));

	np->loop = loop;
	np->ot = ot;
	np->metadata_path = xstrdup(metadata_fifo);
	np->metadata_fd = -1;

	np->registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(np->registry, &np->registry_listener, &registry_events, np);

	np->mpris = mpris_new(loop, &mpris_events, np);

	/* O_RDWR so the fifo never reports EOF when OwnTone closes its end */
	np->remote_fd = open(remote_fifo, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (np->remote_fd >= 0)
		np->remote_io = pw_loop_add_io(loop, np->remote_fd, SPA_IO_IN, false, on_remote_io, np);
	else
		pw_log_warn("can't open %s: %m, remote control from the speakers disabled", remote_fifo);

	pthread_mutex_init(&np->lock, NULL);
	pthread_cond_init(&np->cond, NULL);
	pthread_create(&np->thread, NULL, writer_thread, np);
	return np;
}

void nowplaying_free(struct nowplaying *np)
{
	struct job *job, *next;

	if (!np)
		return;

	pthread_mutex_lock(&np->lock);
	np->quit = true;
	pthread_cond_signal(&np->cond);
	pthread_mutex_unlock(&np->lock);
	pthread_join(np->thread, NULL);
	for (job = np->jobs; job; job = next) {
		next = job->next;
		free(job->data);
		free(job->art_url);
		free(job->suffix);
		free(job);
	}
	pthread_mutex_destroy(&np->lock);
	pthread_cond_destroy(&np->cond);

	if (np->remote_io)
		pw_loop_destroy_source(np->loop, np->remote_io);
	if (np->remote_fd >= 0)
		close(np->remote_fd);

	mpris_free(np->mpris);
	while (np->nodes) {
		struct stream_node *n = np->nodes;
		np->nodes = n->next;
		node_free(n);
	}
	while (np->links) {
		struct link_info *l = np->links;
		np->links = l->next;
		free(l);
	}
	spa_hook_remove(&np->registry_listener);
	pw_proxy_destroy((struct pw_proxy *)np->registry);

	free(np->player);
	free(np->sent_key);
	free(np->sent_status);
	free(np->art_cache_url);
	free(np->art_cache_item);
	free(np->metadata_path);
	free(np);
}

void nowplaying_set_owner(struct nowplaying *np, struct sink *owner)
{
	if (np->owner == owner)
		return;
	np->owner = owner;
	np->ot_paused = false;
	free(np->sent_key);
	np->sent_key = NULL;
	free(np->sent_status);
	np->sent_status = NULL;

	if (!owner) {
		free(np->player);
		np->player = NULL;
		/* Make the writer drop its fd, OwnTone closes the pipe on stop */
		queue_simple(np, JOB_CLOSE);
		return;
	}
	update(np);
}

bool nowplaying_tick(struct nowplaying *np, uint64_t now)
{
	struct mpris_player *p;
	uint64_t last_audio;
	bool app_paused;

	if (!np->owner)
		return false;

	/* Until OwnTone has opened the metadata pipe, keep knocking */
	if (!atomic_load(&np->connected) && elapsed_ns(now, np->last_probe) > 500 * SPA_NSEC_PER_MSEC) {
		np->last_probe = now;
		queue_simple(np, JOB_OPEN);
	}
	if (atomic_exchange(&np->resync, false)) {
		free(np->sent_key);
		np->sent_key = NULL;
		update(np);
	}

	/* The links to our sink settle after playback starts, and players
	 * appear late: retry finding one */
	if (!np->player)
		update(np);

	p = mpris_find(np->mpris, np->player);
	app_paused = p && p->status && strcmp(p->status, "Playing") != 0;
	last_audio = sink_last_audio_ns(np->owner);

	if (!np->ot_paused && app_paused && elapsed_ns(now, last_audio) > PAUSE_DELAY_NS) {
		if (ot_player_pause(np->ot) == 0) {
			pw_log_info("application paused, pausing AirPlay");
			np->ot_paused = true;
			np->paused_since = now;
		}
	} else if (np->ot_paused && (elapsed_ns(now, last_audio) < RECENT_AUDIO_NS || !app_paused)) {
		if (ot_player_play(np->ot) == 0) {
			pw_log_info("resuming AirPlay");
			np->ot_paused = false;
		}
	}
	return np->ot_paused;
}
