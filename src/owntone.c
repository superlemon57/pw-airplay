/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pw-airplay - client for the OwnTone JSON API */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <json-c/json.h>

#include "util.h"
#include "owntone.h"

struct ot_client {
	CURL *curl;
	char base[64];
	char *body;
	size_t body_len;
};

static size_t on_write(char *data, size_t size, size_t nmemb, void *user)
{
	struct ot_client *ot = user;
	size_t len = size * nmemb;
	char *p = realloc(ot->body, ot->body_len + len + 1);

	if (!p)
		return 0;
	ot->body = p;
	memcpy(ot->body + ot->body_len, data, len);
	ot->body_len += len;
	ot->body[ot->body_len] = '\0';
	return len;
}

struct ot_client *ot_client_new(int port)
{
	struct ot_client *ot = xcalloc(1, sizeof(*ot));

	ot->curl = curl_easy_init();
	if (!ot->curl) {
		free(ot);
		return NULL;
	}
	snprintf(ot->base, sizeof(ot->base), "http://127.0.0.1:%d", port);
	return ot;
}

void ot_client_free(struct ot_client *ot)
{
	if (!ot)
		return;
	curl_easy_cleanup(ot->curl);
	free(ot->body);
	free(ot);
}

/* Runs a request against the local API. The response body is in ot->body */
static int request(struct ot_client *ot, const char *method, const char *path, const char *body)
{
	struct curl_slist *headers = NULL;
	char url[512];
	long status = 0;
	CURLcode res;

	snprintf(url, sizeof(url), "%s%s", ot->base, path);
	free(ot->body);
	ot->body = NULL;
	ot->body_len = 0;

	curl_easy_reset(ot->curl);
	curl_easy_setopt(ot->curl, CURLOPT_URL, url);
	curl_easy_setopt(ot->curl, CURLOPT_CUSTOMREQUEST, method);
	curl_easy_setopt(ot->curl, CURLOPT_WRITEFUNCTION, on_write);
	curl_easy_setopt(ot->curl, CURLOPT_WRITEDATA, ot);
	curl_easy_setopt(ot->curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);
	curl_easy_setopt(ot->curl, CURLOPT_TIMEOUT_MS, 3000L);
	curl_easy_setopt(ot->curl, CURLOPT_NOSIGNAL, 1L);
	if (body) {
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(ot->curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(ot->curl, CURLOPT_POSTFIELDS, body);
	}

	res = curl_easy_perform(ot->curl);
	curl_slist_free_all(headers);
	if (res != CURLE_OK)
		return -1;

	curl_easy_getinfo(ot->curl, CURLINFO_RESPONSE_CODE, &status);
	return (status >= 200 && status < 300) ? 0 : -(int)status;
}

static const char *json_str(json_object *obj, const char *key)
{
	json_object *v;

	if (!json_object_object_get_ex(obj, key, &v))
		return NULL;
	return json_object_get_string(v);
}

int ot_get_outputs(struct ot_client *ot, struct ot_output **outputs, int *n)
{
	json_object *root, *arr;
	int res, len;

	*outputs = NULL;
	*n = 0;

	if ((res = request(ot, "GET", "/api/outputs", NULL)) < 0)
		return res;

	root = json_tokener_parse(ot->body ? ot->body : "");
	if (!root)
		return -1;
	if (!json_object_object_get_ex(root, "outputs", &arr) || !json_object_is_type(arr, json_type_array)) {
		json_object_put(root);
		return -1;
	}

	len = json_object_array_length(arr);
	*outputs = xcalloc(len ? len : 1, sizeof(struct ot_output));
	for (int i = 0; i < len; i++) {
		json_object *o = json_object_array_get_idx(arr, i), *v;
		struct ot_output *out = &(*outputs)[*n];
		const char *id = json_str(o, "id"), *name = json_str(o, "name"), *type = json_str(o, "type");

		if (!id || !name)
			continue;
		out->id = xstrdup(id);
		out->name = xstrdup(name);
		out->type = xstrdup(type ? type : "");
		out->selected = json_object_object_get_ex(o, "selected", &v) && json_object_get_boolean(v);
		out->volume = json_object_object_get_ex(o, "volume", &v) ? json_object_get_int(v) : 0;
		(*n)++;
	}

	json_object_put(root);
	return 0;
}

void ot_free_outputs(struct ot_output *outputs, int n)
{
	for (int i = 0; i < n; i++) {
		free(outputs[i].id);
		free(outputs[i].name);
		free(outputs[i].type);
	}
	free(outputs);
}

int ot_set_outputs(struct ot_client *ot, char *const *ids, int n)
{
	json_object *root = json_object_new_object(), *arr = json_object_new_array();
	int res;

	for (int i = 0; i < n; i++)
		json_object_array_add(arr, json_object_new_string(ids[i]));
	json_object_object_add(root, "outputs", arr);

	res = request(ot, "PUT", "/api/outputs/set", json_object_to_json_string(root));
	json_object_put(root);
	return res;
}

int ot_set_volume(struct ot_client *ot, const char *id, int volume)
{
	char path[128], body[64];

	snprintf(path, sizeof(path), "/api/outputs/%s", id);
	snprintf(body, sizeof(body), "{\"volume\":%d}", volume);
	return request(ot, "PUT", path, body);
}

int ot_get_player_state(struct ot_client *ot, char *state, int len)
{
	json_object *root;
	const char *s;
	int res;

	if ((res = request(ot, "GET", "/api/player", NULL)) < 0)
		return res;

	root = json_tokener_parse(ot->body ? ot->body : "");
	if (!root)
		return -1;
	s = json_str(root, "state");
	snprintf(state, len, "%s", s ? s : "");
	json_object_put(root);
	return 0;
}

int ot_player_stop(struct ot_client *ot)
{
	return request(ot, "PUT", "/api/player/stop", NULL);
}

int ot_player_pause(struct ot_client *ot)
{
	return request(ot, "PUT", "/api/player/pause", NULL);
}

int ot_player_play(struct ot_client *ot)
{
	return request(ot, "PUT", "/api/player/play", NULL);
}
