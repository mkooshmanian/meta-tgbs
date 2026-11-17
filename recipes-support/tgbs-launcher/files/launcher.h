/* SPDX-License-Identifier: MIT */
#ifndef TGBS_LAUNCHER_H
#define TGBS_LAUNCHER_H

#include <tgbs/domain.h>
#include <tgbs/channel.h>

struct launcher_config {
	/* String pointers in the specifications borrow storage from document. */
	struct json_object *document;
	tgbs_domain_spec_t *domains;
	size_t domain_count;
	tgbs_channel_spec_t *channels;
	size_t channel_count;
};

int launcher_config_load(int fd, struct launcher_config *config);
void launcher_config_free(struct launcher_config *config);

#endif
