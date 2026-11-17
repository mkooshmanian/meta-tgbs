/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "launcher.h"

#include <errno.h>
#include <limits.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONFIG_MAX_SIZE (1024 * 1024)

static int invalid(const char *where, const char *field, const char *reason)
{
	fprintf(stderr, "tgbs-launcher: %s%s%s: %s\n", where,
		field ? "." : "", field ? field : "", reason);
	return -1;
}

static int keys(struct json_object *object, const char *where,
	const char *const allowed[])
{
	if (!json_object_is_type(object, json_type_object))
		return invalid(where, NULL, "expected an object");
	json_object_object_foreach(object, key, value) {
		(void)value;
		size_t i;
		for (i = 0; allowed[i]; i++)
			if (strcmp(key, allowed[i]) == 0)
				break;
		if (!allowed[i])
			return invalid(where, key, "unknown field");
	}
	return 0;
}

static int field(struct json_object *object, const char *key, enum json_type type,
	int required, const char *where, struct json_object **out)
{
	*out = NULL;
	if (!json_object_object_get_ex(object, key, out))
		return required ? invalid(where, key, "missing field") : 0;
	if (!json_object_is_type(*out, type))
		return invalid(where, key, "unexpected type");
	return 0;
}

static int string_value(struct json_object *value, int empty, const char **out)
{
	if (!json_object_is_type(value, json_type_string))
		return -1;
	*out = json_object_get_string(value);
	return ((!empty && !(*out)[0]) ||
		strlen(*out) != (size_t)json_object_get_string_len(value)) ? -1 : 0;
}

static int string_field(struct json_object *object, const char *key, int required,
	const char *where, const char **out)
{
	struct json_object *value;
	*out = NULL;
	if (field(object, key, json_type_string, required, where, &value))
		return -1;
	if (value && string_value(value, 0, out))
		return invalid(where, key, "expected a nonempty string without NUL bytes");
	return 0;
}

static int positive(struct json_object *value, uint64_t *out)
{
	/* Restrict JSON numbers to signed 64-bit integers. This also rejects
	 * numbers that json-c saturated after an integer overflow. */
	if (!json_object_is_type(value, json_type_int) ||
		json_object_get_int64(value) <= 0 ||
		json_object_get_uint64(value) > INT64_MAX)
		return -1;
	*out = json_object_get_uint64(value);
	return 0;
}

static int number_field(struct json_object *object, const char *key, int required,
	const char *where, uint64_t *out)
{
	struct json_object *value;
	if (!json_object_object_get_ex(object, key, &value))
		return required ? invalid(where, key, "missing field") : 0;
	if (positive(value, out))
		return invalid(where, key, "expected an integer between 1 and INT64_MAX");
	return 0;
}

static int bool_field(struct json_object *object, const char *key,
	const char *where, int *out)
{
	struct json_object *value;
	if (field(object, key, json_type_boolean, 0, where, &value))
		return -1;
	if (value)
		*out = json_object_get_boolean(value);
	return 0;
}

static int limit_field(struct json_object *object, const char *key,
	const char *where, tgbs_limit_t *out)
{
	struct json_object *value;
	const char *text;
	if (!json_object_object_get_ex(object, key, &value))
		return 0;
	if (!string_value(value, 0, &text) && strcmp(text, "max") == 0) {
		out->unlimited = 1;
		return 0;
	}
	if (positive(value, &out->value))
		return invalid(where, key, "expected a positive integer or \"max\"");
	out->unlimited = 0;
	return 0;
}

static int valid_name(const char *text, size_t capacity)
{
	return text && text[0] && strlen(text) < capacity &&
		strcmp(text, ".") && strcmp(text, "..") &&
		strspn(text, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") == strlen(text);
}

static int valid_cpus(const char *text)
{
	if (!text || strcmp(text, "inherit") == 0)
		return 1;
	while (*text) {
		char *end;
		unsigned long first, last;
		if (*text < '0' || *text > '9')
			return 0;
		errno = 0;
		first = strtoul(text, &end, 10);
		if (errno || first > INT_MAX)
			return 0;
		text = end;
		if (*text == '-') {
			text++;
			if (*text < '0' || *text > '9')
				return 0;
			last = strtoul(text, &end, 10);
			if (errno || last > INT_MAX || last < first)
				return 0;
			text = end;
		}
		if (!*text)
			return 1;
		if (*text++ != ',' || !*text)
			return 0;
	}
	return 0;
}

static int parse_extra(struct json_object *object, const char *where,
	tgbs_domain_spec_t *spec)
{
	struct json_object *extra, *binds;
	if (field(object, "extra", json_type_object, 0, where, &extra))
		return -1;
	if (!extra)
		return 0;
	if (keys(extra, where, (const char *[]) {"binds", "ephemeral", NULL}) ||
		bool_field(extra, "ephemeral", where, &spec->ephemeral) ||
		field(extra, "binds", json_type_array, 0, where, &binds))
		return -1;
	if (!binds)
		return 0;
	spec->bind_count = json_object_array_length(binds);
	tgbs_bind_t *entries = calloc(spec->bind_count ? spec->bind_count : 1, sizeof(*entries));
	if (!entries)
		return -1;
	spec->binds = entries;
	for (size_t i = 0; i < spec->bind_count; i++) {
		struct json_object *bind = json_object_array_get_idx(binds, i);
		char location[128];
		snprintf(location, sizeof(location), "%s.extra.binds[%zu]", where, i);
		if (keys(bind, location, (const char *[]) {"source", "destination", "read_only", NULL}) ||
			string_field(bind, "source", 1, location, &entries[i].source) ||
			string_field(bind, "destination", 1, location, &entries[i].destination) ||
			bool_field(bind, "read_only", location, &entries[i].read_only))
			return -1;
		if (entries[i].source[0] != '/' || entries[i].destination[0] != '/' ||
			strlen(entries[i].source) >= PATH_MAX || strlen(entries[i].destination) >= PATH_MAX)
			return invalid(location, NULL, "bind paths must be absolute and shorter than PATH_MAX");
	}
	return 0;
}

static int parse_domain(struct json_object *object, const char *where,
	tgbs_domain_spec_t *spec)
{
	struct json_object *args;
	const char *entrypoint;
	*spec = (tgbs_domain_spec_t)TGBS_DOMAIN_SPEC_INIT;
	if (keys(object, where, (const char *[]) {"name", "entrypoint", "args", "runtime_us",
		"period_us", "cpus", "reclaim", "memory_max", "pids_max", "extra", NULL}) ||
		string_field(object, "name", 1, where, &spec->name) ||
		string_field(object, "entrypoint", 1, where, &entrypoint) ||
		string_field(object, "cpus", 0, where, &spec->cpus) ||
		number_field(object, "runtime_us", 1, where, &spec->runtime_us) ||
		number_field(object, "period_us", 1, where, &spec->period_us) ||
		bool_field(object, "reclaim", where, &spec->reclaim) ||
		limit_field(object, "memory_max", where, &spec->memory_max) ||
		limit_field(object, "pids_max", where, &spec->pids_max) ||
		field(object, "args", json_type_array, 0, where, &args))
		return -1;
	if (!valid_name(spec->name, TGBS_DOMAIN_NAME_SIZE) ||
		!strcmp(spec->name, "channels") || !strcmp(spec->name, "rootfs") ||
		!strcmp(spec->name, "host") || !strcmp(spec->name, "system"))
		return invalid(where, "name", "invalid or reserved domain name");
	if (entrypoint[0] != '/' || strlen(entrypoint) >= PATH_MAX)
		return invalid(where, "entrypoint", "expected an absolute executable path");
	if (spec->runtime_us > spec->period_us)
		return invalid(where, "runtime_us", "must not exceed period_us");
	if (!valid_cpus(spec->cpus))
		return invalid(where, "cpus", "expected inherit or a CPU list such as 0,2-3");
	size_t count = args ? json_object_array_length(args) : 0;
	char **argv = calloc(count + 2, sizeof(*argv));
	if (!argv)
		return -1;
	spec->argv = argv;
	argv[0] = (char *)entrypoint;
	for (size_t i = 0; i < count; i++) {
		const char *arg;
		if (string_value(json_object_array_get_idx(args, i), 1, &arg))
			return invalid(where, "args", "expected strings without NUL bytes");
		argv[i + 1] = (char *)arg;
	}
	return parse_extra(object, where, spec);
}

static const char *canonical(const char *name)
{
	return !strcmp(name, "system") ? "host" : name;
}

static int participant(const struct launcher_config *config, const char *name)
{
	if (!strcmp(canonical(name), "host"))
		return 1;
	for (size_t i = 0; i < config->domain_count; i++)
		if (!strcmp(name, config->domains[i].name))
			return 1;
	return 0;
}

static int parse_channel(struct json_object *object, const char *where,
	tgbs_channel_spec_t *spec, const struct launcher_config *config)
{
	struct json_object *destinations;
	const char *type;
	if (keys(object, where, (const char *[]) {"name", "type", "source", "destinations",
		"max_message_size", "refresh_period_us", NULL}) ||
		string_field(object, "name", 1, where, &spec->name) ||
		string_field(object, "type", 1, where, &type) ||
		string_field(object, "source", 1, where, &spec->source) ||
		number_field(object, "max_message_size", 1, where, &spec->max_message_size) ||
		number_field(object, "refresh_period_us", 0, where, &spec->refresh_period_us) ||
		field(object, "destinations", json_type_array, 1, where, &destinations))
		return -1;
	if (!strcmp(type, "queuing"))
		spec->type = TGBS_CHANNEL_QUEUING;
	else if (!strcmp(type, "sampling"))
		spec->type = TGBS_CHANNEL_SAMPLING;
	else
		return invalid(where, "type", "expected queuing or sampling");
	if (!valid_name(spec->name, TGBS_CHANNEL_NAME_SIZE))
		return invalid(where, "name", "invalid channel name");
	if (!participant(config, spec->source))
		return invalid(where, "source", "expected a configured domain or host");
	spec->destination_count = json_object_array_length(destinations);
	if (!spec->destination_count || spec->destination_count > TGBS_CHANNEL_MAX_DESTINATIONS ||
		(spec->type == TGBS_CHANNEL_QUEUING && spec->destination_count != 1))
		return invalid(where, "destinations", "queuing needs one destination; sampling needs 1 to 64");
	if (spec->max_message_size > INT_MAX - 32 ||
		(spec->type == TGBS_CHANNEL_QUEUING && spec->refresh_period_us) ||
		(spec->type == TGBS_CHANNEL_SAMPLING &&
		 (!spec->refresh_period_us || spec->refresh_period_us > UINT64_MAX / 1000)))
		return invalid(where, NULL, "invalid message size or refresh period for channel type");
	const char **entries = calloc(spec->destination_count, sizeof(*entries));
	if (!entries)
		return -1;
	spec->destinations = entries;
	for (size_t i = 0; i < spec->destination_count; i++) {
		if (string_value(json_object_array_get_idx(destinations, i), 0, &entries[i]) ||
			!participant(config, entries[i]))
			return invalid(where, "destinations", "expected configured domains or host");
		for (size_t j = 0; j < i; j++)
			if (!strcmp(canonical(entries[i]), canonical(entries[j])))
				return invalid(where, "destinations", "duplicate destination");
	}
	return 0;
}

int launcher_config_load(int fd, struct launcher_config *config)
{
	char *buffer = malloc(CONFIG_MAX_SIZE + 1);
	struct json_tokener *tokener = json_tokener_new();
	struct json_object *domains, *channels;
	size_t used = 0;
	int result = -1;
	if (!buffer || !tokener)
		goto out;
	while (used < CONFIG_MAX_SIZE + 1) {
		ssize_t n = read(fd, buffer + used, CONFIG_MAX_SIZE + 1 - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			invalid("config", NULL, strerror(errno));
			goto out;
		}
		if (!n)
			break;
		used += n;
	}
	if (used > CONFIG_MAX_SIZE) {
		invalid("config", NULL, "file exceeds 1 MiB");
		goto out;
	}
	buffer[used] = '\0';
	json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	config->document = json_tokener_parse_ex(tokener, buffer, (int)used + 1);
	if (json_tokener_get_error(tokener) != json_tokener_success ||
		json_tokener_get_parse_end(tokener) < used) {
		invalid("config", NULL, "invalid JSON or trailing data");
		goto out;
	}
	uint64_t version = 0;
	if (keys(config->document, "config", (const char *[]) {"version", "domains", "channels", NULL}) ||
		number_field(config->document, "version", 1, "config", &version) ||
		field(config->document, "domains", json_type_array, 1, "config", &domains) ||
		field(config->document, "channels", json_type_array, 0, "config", &channels))
		goto out;
	if (version != 1) {
		invalid("config", "version", "only version 1 is supported");
		goto out;
	}
	config->domain_count = json_object_array_length(domains);
	if (!config->domain_count) {
		invalid("config", "domains", "expected at least one domain");
		goto out;
	}
	config->domains = calloc(config->domain_count, sizeof(*config->domains));
	if (!config->domains)
		goto out;
	for (size_t i = 0; i < config->domain_count; i++) {
		char where[64];
		snprintf(where, sizeof(where), "domains[%zu]", i);
		if (parse_domain(json_object_array_get_idx(domains, i), where, &config->domains[i]))
			goto out;
		for (size_t j = 0; j < i; j++)
			if (!strcmp(config->domains[i].name, config->domains[j].name)) {
				invalid(where, "name", "duplicate domain");
				goto out;
			}
	}
	config->channel_count = channels ? json_object_array_length(channels) : 0;
	config->channels = calloc(config->channel_count ? config->channel_count : 1, sizeof(*config->channels));
	if (!config->channels)
		goto out;
	for (size_t i = 0; i < config->channel_count; i++) {
		char where[64];
		snprintf(where, sizeof(where), "channels[%zu]", i);
		if (parse_channel(json_object_array_get_idx(channels, i), where, &config->channels[i], config))
			goto out;
		for (size_t j = 0; j < i; j++)
			if (!strcmp(config->channels[i].name, config->channels[j].name)) {
				invalid(where, "name", "duplicate channel");
				goto out;
			}
	}
	result = 0;
out:
	free(buffer);
	if (tokener)
		json_tokener_free(tokener);
	if (result)
		launcher_config_free(config);
	return result;
}

void launcher_config_free(struct launcher_config *config)
{
	if (config->domains)
		for (size_t i = 0; i < config->domain_count; i++) {
			free((void *)config->domains[i].argv);
			free((void *)config->domains[i].binds);
		}
	if (config->channels)
		for (size_t i = 0; i < config->channel_count; i++)
			free((void *)config->channels[i].destinations);
	free(config->domains);
	free(config->channels);
	json_object_put(config->document);
	memset(config, 0, sizeof(*config));
}
