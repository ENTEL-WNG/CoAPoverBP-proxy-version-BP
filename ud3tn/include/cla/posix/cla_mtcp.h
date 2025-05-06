// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef CLA_MTCP_H
#define CLA_MTCP_H

#include "cla/cla.h"
#include "cla/posix/cla_tcp_common.h"

#include "ud3tn/bundle.h"
#include "ud3tn/bundle_processor.h"

#include <stddef.h>

// Whether or not to close active TCP connections after a contact
#ifndef CLA_MTCP_CLOSE_AFTER_CONTACT
#define CLA_MTCP_CLOSE_AFTER_CONTACT 1
#endif // CLA_MTCP_CLOSE_AFTER_CONTACT

struct cla_config *mtcp_create(
	const char *const options[], const size_t option_count,
	const struct bundle_agent_interface *bundle_agent_interface);

/* INTERNAL API */

struct mtcp_link {
	struct cla_tcp_link base;
	struct parser mtcp_parser;
};

size_t mtcp_mbs_get(struct cla_config *const config);

void mtcp_reset_parsers(struct cla_link *link);

size_t mtcp_forward_to_specific_parser(struct cla_link *link,
				       const uint8_t *buffer, size_t length);

enum cla_begin_packet_result mtcp_begin_packet(struct cla_link *link,
		       const struct bundle *const bundle,
		       size_t length,
		       char *cla_addr);

enum ud3tn_result mtcp_end_packet(struct cla_link *link);

enum ud3tn_result mtcp_send_packet_data(
	struct cla_link *link, const void *data, const size_t length);

#endif /* CLA_MTCP_H */
