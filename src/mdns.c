/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - speaker grouping from mDNS
 *
 * OwnTone names outputs after each device's service name. Both halves of a
 * HomePod stereo pair are advertised separately, with the same "tsid" and the
 * pair's name (the one shown in the Home app) in "gpn".
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>

#include <pipewire/log.h>

#include "util.h"
#include "mdns.h"

struct entry {
	char *name;
	struct mdns_info info;
	struct entry *next;
};

struct mdns {
	AvahiThreadedPoll *poll;
	AvahiClient *client;
	AvahiServiceBrowser *browser;
	pthread_mutex_t lock;
	struct entry *entries;
};

static char *txt_get(AvahiStringList *txt, const char *key)
{
	AvahiStringList *item = avahi_string_list_find(txt, key);
	char *k = NULL, *v = NULL, *res = NULL;

	if (item && avahi_string_list_get_pair(item, &k, &v, NULL) == 0 && v)
		res = xstrdup(v);
	avahi_free(k);
	avahi_free(v);
	return res;
}

/* Takes ownership of the strings in info */
static void set_entry(struct mdns *m, const char *name, struct mdns_info *info)
{
	struct entry *e;

	pthread_mutex_lock(&m->lock);
	for (e = m->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0)
			break;
	if (!e) {
		e = xcalloc(1, sizeof(*e));
		e->name = xstrdup(name);
		e->next = m->entries;
		m->entries = e;
	}
	mdns_info_clear(&e->info);
	e->info = *info;
	pthread_mutex_unlock(&m->lock);
}

static void on_resolve(AvahiServiceResolver *r, AvahiIfIndex iface, AvahiProtocol proto,
		       AvahiResolverEvent event, const char *name, const char *type,
		       const char *domain, const char *host, const AvahiAddress *addr,
		       uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags flags,
		       void *data)
{
	struct mdns *m = data;

	if (event == AVAHI_RESOLVER_FOUND) {
		struct mdns_info info = { 0 };
		char *igl = txt_get(txt, "igl");
		char *model = txt_get(txt, "model");

		info.group = txt_get(txt, "gpn");
		info.pair_id = txt_get(txt, "tsid");
		/* Devices that don't say are on their own, i.e. lead themselves */
		info.leader = !igl || strcmp(igl, "1") == 0;
		/* Mac14,2, MacBookPro18,1, iMac21,1, Macmini9,1... */
		info.computer = model && (strncmp(model, "Mac", 3) == 0 || strncmp(model, "iMac", 4) == 0);
		free(igl);
		free(model);

		pw_log_info("mdns: '%s' group '%s' pair %s%s%s", name, info.group ? info.group : "-",
			    info.pair_id ? info.pair_id : "-", info.leader ? " (leader)" : "",
			    info.computer ? " (computer)" : "");
		set_entry(m, name, &info);
	}
	avahi_service_resolver_free(r);
}

static void on_browse(AvahiServiceBrowser *b, AvahiIfIndex iface, AvahiProtocol proto,
		      AvahiBrowserEvent event, const char *name, const char *type,
		      const char *domain, AvahiLookupResultFlags flags, void *data)
{
	struct mdns *m = data;

	switch (event) {
	case AVAHI_BROWSER_NEW:
		if (!avahi_service_resolver_new(m->client, iface, proto, name, type, domain,
						AVAHI_PROTO_UNSPEC, 0, on_resolve, m))
			pw_log_warn("mdns: can't resolve '%s': %s", name,
				    avahi_strerror(avahi_client_errno(m->client)));
		break;
	case AVAHI_BROWSER_FAILURE:
		pw_log_warn("mdns: browser failure: %s", avahi_strerror(avahi_client_errno(m->client)));
		break;
	default:
		break;
	}
}

static void on_client(AvahiClient *c, AvahiClientState state, void *data)
{
	struct mdns *m = data;

	m->client = c;
	switch (state) {
	case AVAHI_CLIENT_S_RUNNING:
		if (!m->browser)
			m->browser = avahi_service_browser_new(c, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
							       "_airplay._tcp", NULL, 0, on_browse, m);
		break;
	case AVAHI_CLIENT_FAILURE:
		pw_log_warn("mdns: avahi failure: %s", avahi_strerror(avahi_client_errno(c)));
		break;
	default:
		break;
	}
}

struct mdns *mdns_new(void)
{
	struct mdns *m = xcalloc(1, sizeof(*m));
	int err;

	pthread_mutex_init(&m->lock, NULL);
	m->poll = avahi_threaded_poll_new();
	if (!m->poll)
		goto fail;

	/* NO_FAIL: keep working (and reconnect) if avahi-daemon restarts */
	avahi_threaded_poll_lock(m->poll);
	m->client = avahi_client_new(avahi_threaded_poll_get(m->poll), AVAHI_CLIENT_NO_FAIL,
				     on_client, m, &err);
	avahi_threaded_poll_unlock(m->poll);
	if (!m->client) {
		pw_log_warn("mdns: %s, group names unavailable", avahi_strerror(err));
		goto fail;
	}
	avahi_threaded_poll_start(m->poll);
	return m;

fail:
	mdns_free(m);
	return NULL;
}

void mdns_free(struct mdns *m)
{
	struct entry *e, *next;

	if (!m)
		return;
	if (m->poll)
		avahi_threaded_poll_stop(m->poll);
	if (m->client)
		avahi_client_free(m->client);
	if (m->poll)
		avahi_threaded_poll_free(m->poll);
	for (e = m->entries; e; e = next) {
		next = e->next;
		free(e->name);
		mdns_info_clear(&e->info);
		free(e);
	}
	pthread_mutex_destroy(&m->lock);
	free(m);
}

void mdns_info_clear(struct mdns_info *info)
{
	free(info->group);
	free(info->pair_id);
	memset(info, 0, sizeof(*info));
}

bool mdns_lookup(struct mdns *m, const char *service_name, struct mdns_info *info)
{
	struct entry *e;
	bool found = false;

	memset(info, 0, sizeof(*info));
	if (!m)
		return false;
	pthread_mutex_lock(&m->lock);
	for (e = m->entries; e; e = e->next)
		if (strcmp(e->name, service_name) == 0) {
			*info = e->info;
			info->group = e->info.group ? xstrdup(e->info.group) : NULL;
			info->pair_id = e->info.pair_id ? xstrdup(e->info.pair_id) : NULL;
			found = true;
			break;
		}
	pthread_mutex_unlock(&m->lock);
	return found;
}
