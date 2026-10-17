/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - MPRIS client: what desktop media players are playing
 *
 * Tracks every org.mpris.MediaPlayer2.* on the session bus, with its state and
 * metadata, using sd-bus driven from the PipeWire main loop.
 */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include <systemd/sd-bus.h>

#include "util.h"
#include "mpris.h"

#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_IFACE "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
/* A hung player must not stall us */
#define CALL_TIMEOUT_USEC (500 * 1000)

struct mpris {
	struct pw_loop *loop;
	const struct mpris_events *events;
	void *events_data;

	sd_bus *bus;
	struct spa_source *io;
	struct spa_source *timer;
	bool dirty;

	struct mpris_player *players;
};

static void player_clear_metadata(struct mpris_player *p)
{
	free(p->track_id);
	free(p->title);
	free(p->artist);
	free(p->album);
	free(p->art_url);
	p->track_id = p->title = p->artist = p->album = p->art_url = NULL;
	p->length_us = 0;
}

static void player_free(struct mpris_player *p)
{
	player_clear_metadata(p);
	free(p->bus_name);
	free(p->unique_name);
	free(p->identity);
	free(p->desktop_entry);
	free(p->status);
	free(p);
}

static void set_str(char **dst, const char *value)
{
	free(*dst);
	*dst = (value && *value) ? xstrdup(value) : NULL;
}

/* Reads the current variant as a string-ish value. "o" for track ids. */
static int read_variant_string(sd_bus_message *msg, const char *contents, char **out)
{
	const char *s;
	int r;

	if (strcmp(contents, "s") != 0 && strcmp(contents, "o") != 0)
		return sd_bus_message_skip(msg, "v");
	if ((r = sd_bus_message_enter_container(msg, 'v', contents)) < 0)
		return r;
	if ((r = sd_bus_message_read_basic(msg, contents[0], &s)) < 0)
		return r;
	set_str(out, s);
	return sd_bus_message_exit_container(msg);
}

/* xesam:artist is a list of strings, joined with ", " */
static int read_variant_string_list(sd_bus_message *msg, const char *contents, char **out)
{
	char buf[512] = "";
	const char *s;
	int r;

	if (strcmp(contents, "as") != 0)
		return read_variant_string(msg, contents, out);
	if ((r = sd_bus_message_enter_container(msg, 'v', "as")) < 0)
		return r;
	if ((r = sd_bus_message_enter_container(msg, 'a', "s")) < 0)
		return r;
	while ((r = sd_bus_message_read_basic(msg, 's', &s)) > 0) {
		size_t len = strlen(buf);
		snprintf(buf + len, sizeof(buf) - len, "%s%s", len ? ", " : "", s);
	}
	if (r < 0)
		return r;
	if ((r = sd_bus_message_exit_container(msg)) < 0)
		return r;
	set_str(out, buf);
	return sd_bus_message_exit_container(msg);
}

/* mpris:length should be "x", but players send all sorts of integers */
static int read_variant_int64(sd_bus_message *msg, const char *contents, int64_t *out)
{
	union { int64_t x; uint64_t t; int32_t i; uint32_t u; double d; } v;
	int r;

	if (strlen(contents) != 1 || !strchr("xtiud", contents[0]))
		return sd_bus_message_skip(msg, "v");
	if ((r = sd_bus_message_enter_container(msg, 'v', contents)) < 0)
		return r;
	if ((r = sd_bus_message_read_basic(msg, contents[0], &v)) < 0)
		return r;
	switch (contents[0]) {
	case 'x': *out = v.x; break;
	case 't': *out = (int64_t)v.t; break;
	case 'i': *out = v.i; break;
	case 'u': *out = v.u; break;
	case 'd': *out = (int64_t)v.d; break;
	}
	return sd_bus_message_exit_container(msg);
}

/* Parses an a{sv} dict, calling fn for each entry with the message positioned
 * on the variant. fn must consume the variant. */
static int parse_dict(sd_bus_message *msg, struct mpris_player *p,
		      int (*fn)(sd_bus_message *msg, struct mpris_player *p, const char *key, const char *contents))
{
	const char *key, *contents;
	char type;
	int r;

	if ((r = sd_bus_message_enter_container(msg, 'a', "{sv}")) < 0)
		return r;
	while ((r = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		if ((r = sd_bus_message_read_basic(msg, 's', &key)) < 0)
			return r;
		if ((r = sd_bus_message_peek_type(msg, &type, &contents)) < 0)
			return r;
		if ((r = fn(msg, p, key, contents)) < 0)
			return r;
		if ((r = sd_bus_message_exit_container(msg)) < 0)
			return r;
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(msg);
}

static int on_metadata_entry(sd_bus_message *msg, struct mpris_player *p, const char *key, const char *contents)
{
	if (strcmp(key, "mpris:trackid") == 0)
		return read_variant_string(msg, contents, &p->track_id);
	if (strcmp(key, "xesam:title") == 0)
		return read_variant_string(msg, contents, &p->title);
	if (strcmp(key, "xesam:artist") == 0)
		return read_variant_string_list(msg, contents, &p->artist);
	if (strcmp(key, "xesam:album") == 0)
		return read_variant_string(msg, contents, &p->album);
	if (strcmp(key, "mpris:artUrl") == 0)
		return read_variant_string(msg, contents, &p->art_url);
	if (strcmp(key, "mpris:length") == 0)
		return read_variant_int64(msg, contents, &p->length_us);
	return sd_bus_message_skip(msg, "v");
}

static int on_property(sd_bus_message *msg, struct mpris_player *p, const char *key, const char *contents)
{
	int r;

	if (strcmp(key, "PlaybackStatus") == 0)
		return read_variant_string(msg, contents, &p->status);
	if (strcmp(key, "Identity") == 0)
		return read_variant_string(msg, contents, &p->identity);
	if (strcmp(key, "DesktopEntry") == 0)
		return read_variant_string(msg, contents, &p->desktop_entry);
	if (strcmp(key, "Metadata") == 0 && strcmp(contents, "a{sv}") == 0) {
		player_clear_metadata(p);
		if ((r = sd_bus_message_enter_container(msg, 'v', "a{sv}")) < 0)
			return r;
		if ((r = parse_dict(msg, p, on_metadata_entry)) < 0)
			return r;
		return sd_bus_message_exit_container(msg);
	}
	return sd_bus_message_skip(msg, "v");
}

static void get_all(struct mpris *m, struct mpris_player *p, const char *iface)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;

	if (sd_bus_call_method(m->bus, p->unique_name, MPRIS_PATH, "org.freedesktop.DBus.Properties",
			       "GetAll", &err, &reply, "s", iface) >= 0)
		parse_dict(reply, p, on_property);
	else
		pw_log_debug("mpris: GetAll(%s) on %s: %s", iface, p->bus_name, err.message);
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
}

static struct mpris_player *find_by_unique(struct mpris *m, const char *unique)
{
	for (struct mpris_player *p = m->players; p; p = p->next)
		if (p->unique_name && strcmp(p->unique_name, unique) == 0)
			return p;
	return NULL;
}

struct mpris_player *mpris_find(struct mpris *m, const char *bus_name)
{
	if (!m || !bus_name)
		return NULL;
	for (struct mpris_player *p = m->players; p; p = p->next)
		if (strcmp(p->bus_name, bus_name) == 0)
			return p;
	return NULL;
}

static void remove_player(struct mpris *m, const char *bus_name)
{
	for (struct mpris_player **pp = &m->players; *pp; pp = &(*pp)->next)
		if (strcmp((*pp)->bus_name, bus_name) == 0) {
			struct mpris_player *p = *pp;

			*pp = p->next;
			pw_log_info("mpris: player gone: %s", bus_name);
			player_free(p);
			m->dirty = true;
			return;
		}
}

static void add_player(struct mpris *m, const char *bus_name, const char *unique)
{
	sd_bus_message *reply = NULL;
	struct mpris_player *p;

	remove_player(m, bus_name);

	p = xcalloc(1, sizeof(*p));
	p->bus_name = xstrdup(bus_name);
	p->unique_name = xstrdup(unique);

	/* Used to tell which player feeds which PipeWire stream */
	if (sd_bus_call_method(m->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			       "org.freedesktop.DBus", "GetConnectionUnixProcessID", NULL, &reply,
			       "s", unique) >= 0)
		sd_bus_message_read(reply, "u", &p->pid);
	sd_bus_message_unref(reply);

	get_all(m, p, MPRIS_IFACE);
	get_all(m, p, MPRIS_PLAYER_IFACE);

	p->next = m->players;
	m->players = p;
	m->dirty = true;
	pw_log_info("mpris: player %s (%s, pid %u): %s", bus_name, p->identity ? p->identity : "?",
		    p->pid, p->status ? p->status : "?");
}

static int on_name_owner_changed(sd_bus_message *msg, void *data, sd_bus_error *ret_error)
{
	struct mpris *m = data;
	const char *name, *old_owner, *new_owner;

	if (sd_bus_message_read(msg, "sss", &name, &old_owner, &new_owner) < 0)
		return 0;
	if (strncmp(name, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) != 0)
		return 0;

	if (*new_owner)
		add_player(m, name, new_owner);
	else
		remove_player(m, name);
	return 0;
}

static int on_properties_changed(sd_bus_message *msg, void *data, sd_bus_error *ret_error)
{
	struct mpris *m = data;
	struct mpris_player *p = find_by_unique(m, sd_bus_message_get_sender(msg));
	const char *iface;

	if (!p || sd_bus_message_read(msg, "s", &iface) < 0)
		return 0;
	if (strcmp(iface, MPRIS_PLAYER_IFACE) != 0 && strcmp(iface, MPRIS_IFACE) != 0)
		return 0;

	/* Some players only list properties as invalidated, others send partial
	 * metadata. Fetching everything again is simplest and always right. */
	get_all(m, p, iface);
	m->dirty = true;
	return 0;
}

static int on_seeked(sd_bus_message *msg, void *data, sd_bus_error *ret_error)
{
	struct mpris *m = data;
	struct mpris_player *p = find_by_unique(m, sd_bus_message_get_sender(msg));

	if (p) {
		p->seek_serial++;
		m->dirty = true;
	}
	return 0;
}

static void list_players(struct mpris *m)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	char **names = NULL;

	if (sd_bus_call_method(m->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			       "org.freedesktop.DBus", "ListNames", &err, &reply, "") < 0) {
		pw_log_warn("mpris: ListNames: %s", err.message);
		goto out;
	}
	if (sd_bus_message_read_strv(reply, &names) < 0)
		goto out;

	for (char **n = names; *n; n++) {
		sd_bus_message *owner = NULL;
		const char *unique;

		if (strncmp(*n, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) != 0)
			continue;
		if (sd_bus_call_method(m->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
				       "org.freedesktop.DBus", "GetNameOwner", NULL, &owner, "s", *n) >= 0 &&
		    sd_bus_message_read(owner, "s", &unique) >= 0)
			add_player(m, *n, unique);
		sd_bus_message_unref(owner);
	}

out:
	for (char **n = names; n && *n; n++)
		free(*n);
	free(names);
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
}

/* sd-bus wants to be woken up on its fd and at its timeout */
static void update_io(struct mpris *m)
{
	int events = sd_bus_get_events(m->bus);
	uint32_t mask = SPA_IO_ERR | SPA_IO_HUP;
	uint64_t usec;

	if (events & POLLIN)
		mask |= SPA_IO_IN;
	if (events & POLLOUT)
		mask |= SPA_IO_OUT;
	pw_loop_update_io(m->loop, m->io, mask);

	if (sd_bus_get_timeout(m->bus, &usec) >= 0 && usec != UINT64_MAX) {
		struct timespec ts = { usec / 1000000, (usec % 1000000) * 1000 };

		if (usec == 0)
			ts.tv_nsec = 1;
		pw_loop_update_timer(m->loop, m->timer, &ts, NULL, true);
	} else {
		pw_loop_update_timer(m->loop, m->timer, NULL, NULL, false);
	}
}

static void dispatch(struct mpris *m)
{
	int r;

	while ((r = sd_bus_process(m->bus, NULL)) > 0)
		;
	if (r < 0)
		pw_log_warn("mpris: bus error: %s", strerror(-r));
	update_io(m);

	/* Emit after processing, never from inside sd-bus callbacks */
	if (m->dirty) {
		m->dirty = false;
		if (m->events && m->events->changed)
			m->events->changed(m->events_data);
	}
}

static void on_io(void *data, int fd, uint32_t mask)
{
	dispatch(data);
}

static void on_timer(void *data, uint64_t expirations)
{
	dispatch(data);
}

struct mpris *mpris_new(struct pw_loop *loop, const struct mpris_events *events, void *data)
{
	struct mpris *m = xcalloc(1, sizeof(*m));
	int r;

	m->loop = loop;
	m->events = events;
	m->events_data = data;

	if ((r = sd_bus_open_user(&m->bus)) < 0) {
		pw_log_warn("mpris: no session bus (%s), now playing info disabled", strerror(-r));
		free(m);
		return NULL;
	}
	sd_bus_set_method_call_timeout(m->bus, CALL_TIMEOUT_USEC);

	sd_bus_match_signal(m->bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			    "org.freedesktop.DBus", "NameOwnerChanged", on_name_owner_changed, m);
	sd_bus_match_signal(m->bus, NULL, NULL, MPRIS_PATH, "org.freedesktop.DBus.Properties",
			    "PropertiesChanged", on_properties_changed, m);
	sd_bus_match_signal(m->bus, NULL, NULL, MPRIS_PATH, MPRIS_PLAYER_IFACE, "Seeked", on_seeked, m);

	m->io = pw_loop_add_io(loop, sd_bus_get_fd(m->bus), SPA_IO_IN, false, on_io, m);
	m->timer = pw_loop_add_timer(loop, on_timer, m);

	list_players(m);
	m->dirty = false;
	update_io(m);
	return m;
}

void mpris_free(struct mpris *m)
{
	struct mpris_player *p, *next;

	if (!m)
		return;
	pw_loop_destroy_source(m->loop, m->io);
	pw_loop_destroy_source(m->loop, m->timer);
	for (p = m->players; p; p = next) {
		next = p->next;
		player_free(p);
	}
	sd_bus_flush_close_unref(m->bus);
	free(m);
}

struct mpris_player *mpris_players(struct mpris *m)
{
	return m ? m->players : NULL;
}

int mpris_position(struct mpris *m, struct mpris_player *p, int64_t *pos_us)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	int r;

	r = sd_bus_get_property_trivial(m->bus, p->unique_name, MPRIS_PATH, MPRIS_PLAYER_IFACE,
					"Position", &err, 'x', pos_us);
	sd_bus_error_free(&err);
	update_io(m);
	return r < 0 ? r : 0;
}

void mpris_command(struct mpris *m, struct mpris_player *p, const char *method)
{
	int r;

	r = sd_bus_call_method_async(m->bus, NULL, p->unique_name, MPRIS_PATH, MPRIS_PLAYER_IFACE,
				     method, NULL, NULL, "");
	if (r < 0)
		pw_log_warn("mpris: %s on %s: %s", method, p->bus_name, strerror(-r));
	else
		pw_log_info("mpris: %s -> %s", method, p->bus_name);
	update_io(m);
}
