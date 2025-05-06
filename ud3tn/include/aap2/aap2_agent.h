// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef AAP2_AGENT_H_INCLUDED
#define AAP2_AGENT_H_INCLUDED

#include "ud3tn/bundle_processor.h"

#include <stdint.h>

// Backlog value for listen() in the AAP 2.0 agent.
#ifndef AAP2_AGENT_LISTEN_BACKLOG
#define AAP2_AGENT_LISTEN_BACKLOG 2
#endif // AAP2_AGENT_LISTEN_BACKLOG

// Maximum number of items waiting in the queue toward an AAP2 agent.
#ifndef AAP2_AGENT_QUEUE_LENGTH
#define AAP2_AGENT_QUEUE_LENGTH 10
#endif // AAP2_AGENT_QUEUE_LENGTH

// Timeout for expecting a response from the Client.
#ifndef AAP2_AGENT_TIMEOUT_MS
#define AAP2_AGENT_TIMEOUT_MS 1000
#endif // AAP2_AGENT_TIMEOUT_MS

struct aap2_agent_config *aap2_agent_setup(
	const struct bundle_agent_interface *bundle_agent_interface,
	const char *socket_path,
	const char *node, const char *service,
	uint8_t bp_version, uint64_t lifetime_ms,
	uint64_t max_bundle_size_bytes);

void aap2_agent_terminate(struct aap2_agent_config *const config);

#endif /* AAP2_AGENT_H_INCLUDED */
