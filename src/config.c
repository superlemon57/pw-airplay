/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - AirPlay 2 sender integrated with PipeWire */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "util.h"
#include "config.h"

static char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

/* Splits "a, b, c" into a newly allocated array */
static char **split_list(const char *value, int *n)
{
	char *copy = xstrdup(value), *tok, *save = NULL;
	char **list = NULL;

	*n = 0;
	for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		tok = trim(tok);
		if (*tok == '\0')
			continue;
		list = xrealloc(list, sizeof(char *) * (*n + 1));
		list[(*n)++] = xstrdup(tok);
	}
	free(copy);
	return list;
}

static void free_list(char **list, int n)
{
	for (int i = 0; i < n; i++)
		free(list[i]);
	free(list);
}

static bool parse_bool(const char *v)
{
	return strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
	       strcasecmp(v, "on") == 0 || strcmp(v, "1") == 0;
}

/* The machine's pretty hostname (as set in system settings), else hostname */
static char *default_sender_name(void)
{
	char line[512], name[256] = "";
	FILE *f = fopen("/etc/machine-info", "r");

	while (f && fgets(line, sizeof(line), f)) {
		if (strncmp(line, "PRETTY_HOSTNAME=", 16) == 0) {
			char *v = trim(line + 16);
			size_t len = strlen(v);

			if (len >= 2 && (v[0] == '"' || v[0] == '\'') && v[len - 1] == v[0]) {
				v[len - 1] = '\0';
				v++;
			}
			snprintf(name, sizeof(name), "%s", v);
		}
	}
	if (f)
		fclose(f);
	if (!*name && gethostname(name, sizeof(name) - 1) < 0)
		snprintf(name, sizeof(name), "Linux");
	return xstrdup(name);
}

static void set_defaults(struct config *cfg)
{
	const char *home = getenv("HOME");
	const char *xdg_data = getenv("XDG_DATA_HOME");
	char buf[4096];

	memset(cfg, 0, sizeof(*cfg));
	cfg->idle_timeout_s = 30;
	cfg->pause_timeout_s = 300;
	cfg->now_playing = true;
	cfg->start_buffer_ms = 2250;
	cfg->pipe_start_buffer_ms = 100;
	cfg->latency_offset_ms = 0;
	cfg->api_port = 3689;
	cfg->all_speakers_sink = true;
	cfg->sender_name = default_sender_name();
	cfg->engine_log_level = xstrdup("log");
	cfg->all_speakers_name = xstrdup("All speakers");

	if (xdg_data && *xdg_data)
		snprintf(buf, sizeof(buf), "%s/pw-airplay", xdg_data);
	else
		snprintf(buf, sizeof(buf), "%s/.local/share/pw-airplay", home ? home : "/tmp");
	cfg->data_dir = xstrdup(buf);
	snprintf(buf, sizeof(buf), "%s/pipes/pw-airplay", cfg->data_dir);
	cfg->fifo_path = xstrdup(buf);
	/* OwnTone reads metadata from the pipe's name + ".metadata" */
	snprintf(buf, sizeof(buf), "%s/pipes/pw-airplay.metadata", cfg->data_dir);
	cfg->metadata_fifo_path = xstrdup(buf);
	/* Not in pipes/, which OwnTone scans for audio */
	snprintf(buf, sizeof(buf), "%s/control/remote", cfg->data_dir);
	cfg->remote_fifo_path = xstrdup(buf);
}

int config_load(struct config *cfg, const char *path)
{
	struct group_cfg *group = NULL, **group_tail;
	char line[4096];
	int lineno = 0;
	FILE *f;

	set_defaults(cfg);
	cfg->config_path = xstrdup(path);
	group_tail = &cfg->groups;

	f = fopen(path, "r");
	if (!f)
		return errno == ENOENT ? 0 : -errno;

	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line), *eq, *key, *value;

		lineno++;
		if (*s == '\0' || *s == '#' || *s == ';')
			continue;

		if (*s == '[') {
			char *end = strchr(s, ']');
			if (!end) {
				fprintf(stderr, "%s:%d: invalid section\n", path, lineno);
				continue;
			}
			*end = '\0';
			s = trim(s + 1);
			group = NULL;
			if (strncasecmp(s, "group ", 6) == 0) {
				char *name = trim(s + 6);
				size_t len = strlen(name);

				if (len >= 2 && name[0] == '"' && name[len - 1] == '"') {
					name[len - 1] = '\0';
					name++;
				}
				group = xcalloc(1, sizeof(*group));
				group->name = xstrdup(name);
				*group_tail = group;
				group_tail = &group->next;
			}
			continue;
		}

		eq = strchr(s, '=');
		if (!eq) {
			fprintf(stderr, "%s:%d: expected key = value\n", path, lineno);
			continue;
		}
		*eq = '\0';
		key = trim(s);
		value = trim(eq + 1);

		if (group) {
			if (strcmp(key, "members") == 0) {
				free_list(group->members, group->n_members);
				group->members = split_list(value, &group->n_members);
			}
			continue;
		}

		if (strcmp(key, "idle_timeout") == 0)
			cfg->idle_timeout_s = atoi(value);
		else if (strcmp(key, "pause_timeout") == 0)
			cfg->pause_timeout_s = atoi(value);
		else if (strcmp(key, "now_playing") == 0)
			cfg->now_playing = parse_bool(value);
		else if (strcmp(key, "start_buffer_ms") == 0)
			cfg->start_buffer_ms = atoi(value);
		else if (strcmp(key, "pipe_start_buffer_ms") == 0)
			cfg->pipe_start_buffer_ms = atoi(value);
		else if (strcmp(key, "latency_offset_ms") == 0)
			cfg->latency_offset_ms = atoi(value);
		else if (strcmp(key, "api_port") == 0)
			cfg->api_port = atoi(value);
		else if (strcmp(key, "engine_log_level") == 0) {
			if (strcmp(value, "log") == 0 || strcmp(value, "info") == 0 || strcmp(value, "debug") == 0) {
				free(cfg->engine_log_level);
				cfg->engine_log_level = xstrdup(value);
			} else
				fprintf(stderr, "%s:%d: engine_log_level must be log, info or debug\n", path, lineno);
		} else if (strcmp(key, "sender_name") == 0 && *value) {
			free(cfg->sender_name);
			cfg->sender_name = xstrdup(value);
		} else if (strcmp(key, "all_speakers_sink") == 0)
			cfg->all_speakers_sink = parse_bool(value);
		else if (strcmp(key, "all_speakers_name") == 0) {
			free(cfg->all_speakers_name);
			cfg->all_speakers_name = xstrdup(value);
		} else if (strcmp(key, "hide") == 0) {
			free_list(cfg->hidden, cfg->n_hidden);
			cfg->hidden = split_list(value, &cfg->n_hidden);
		} else
			fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineno, key);
	}
	fclose(f);

	/* AirPlay devices ignore a start buffer below their own 250 ms latency */
	if (cfg->start_buffer_ms < 500)
		cfg->start_buffer_ms = 500;
	if (cfg->pipe_start_buffer_ms < 20)
		cfg->pipe_start_buffer_ms = 20;
	if (cfg->idle_timeout_s < 1)
		cfg->idle_timeout_s = 1;
	if (cfg->pause_timeout_s < cfg->idle_timeout_s)
		cfg->pause_timeout_s = cfg->idle_timeout_s;
	return 0;
}

void config_free(struct config *cfg)
{
	struct group_cfg *g, *next;

	for (g = cfg->groups; g; g = next) {
		next = g->next;
		free(g->name);
		free_list(g->members, g->n_members);
		free(g);
	}
	free_list(cfg->hidden, cfg->n_hidden);
	free(cfg->all_speakers_name);
	free(cfg->sender_name);
	free(cfg->engine_log_level);
	free(cfg->config_path);
	free(cfg->data_dir);
	free(cfg->fifo_path);
	free(cfg->metadata_fifo_path);
	free(cfg->remote_fifo_path);
}

bool config_is_hidden(const struct config *cfg, const char *name)
{
	for (int i = 0; i < cfg->n_hidden; i++)
		if (strcasecmp(cfg->hidden[i], name) == 0)
			return true;
	return false;
}

/* The engine config is derived from ours, so there is a single place to edit */
int config_write_owntone_conf(const struct config *cfg, const char *path)
{
	const char *user = getenv("USER");
	char tmp[4096], password[33], sender[256];
	int fd;
	unsigned char rnd[16];
	FILE *f;

	f = fopen("/dev/urandom", "r");
	if (!f || fread(rnd, 1, sizeof(rnd), f) != sizeof(rnd)) {
		if (f)
			fclose(f);
		return -EIO;
	}
	fclose(f);
	for (int i = 0; i < 16; i++)
		sprintf(password + i * 2, "%02x", rnd[i]);

	/* libconfuse string: no quotes or backslashes */
	{
		size_t j = 0;

		for (const char *p = cfg->sender_name; *p && j + 1 < sizeof(sender); p++)
			if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20)
				sender[j++] = *p;
		sender[j] = '\0';
	}

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	/* It holds the library password */
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || fchmod(fd, 0600) < 0 || !(f = fdopen(fd, "w"))) {
		int err = errno;

		if (fd >= 0)
			close(fd);
		return -err;
	}

	fprintf(f,
		"# Generated by pw-airplayd from %s - do not edit, changes are overwritten.\n"
		"general {\n"
		"\tuid = \"%s\"\n"
		"\tdb_path = \"%s/cache/songs3.db\"\n"
		"\tcache_dir = \"%s/cache\"\n"
		"\tlogfile = \"%s/log/owntone.log\"\n"
		"\tloglevel = %s\n"
		/* The HTTP server must be reachable from the LAN for DACP (speakers
		 * reporting volume changes). The control API only trusts localhost,
		 * and the library share is locked with a throwaway password. */
		"\ttrusted_networks = { \"localhost\" }\n"
		"\twebsocket_port = 0\n"
		"\tmdns_publish = false\n"
		"\tspeaker_autoselect = no\n"
		/* Recent HomePod firmware (27.x) answers 403 to anything else */
		"\tuser_agent = \"AirPlay/999.0.0\"\n"
		"\tstart_buffer_ms = %d\n"
		"}\n"
		"library {\n"
		"\tname = \"%s\"\n"
		"\tpassword = \"%s\"\n"
		"\tport = %d\n"
		"\tdirectories = { \"%s/pipes\" }\n"
		"\tpodcasts = { }\n"
		"\taudiobooks = { }\n"
		"\tcompilations = { }\n"
		"\tpipe_autostart = true\n"
		"\tpipe_sample_rate = 44100\n"
		"\tpipe_bits_per_sample = 16\n"
		"\tpipe_start_buffer_ms = %d\n"
		"\tpipe_remote_fifo = \"%s\"\n"
		"}\n"
		"audio {\n"
		"\ttype = \"disabled\"\n"
		"}\n",
		cfg->config_path, user ? user : "nobody",
		cfg->data_dir, cfg->data_dir, cfg->data_dir, cfg->engine_log_level,
		cfg->start_buffer_ms, sender, password, cfg->api_port, cfg->data_dir,
		cfg->pipe_start_buffer_ms, cfg->now_playing ? cfg->remote_fifo_path : "");

	for (int i = 0; i < cfg->n_hidden; i++)
		fprintf(f, "airplay \"%s\" {\n\texclude = true\n}\n", cfg->hidden[i]);

	if (fclose(f) != 0 || rename(tmp, path) != 0)
		return -errno;
	return 0;
}
