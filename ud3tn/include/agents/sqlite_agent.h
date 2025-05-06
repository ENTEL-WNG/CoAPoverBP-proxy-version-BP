// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SQLITE_AGENT_H_INCLUDED
#define SQLITE_AGENT_H_INCLUDED

#include "platform/posix/hal_types.h"

#include "ud3tn/bundle_processor.h"

#include <stdint.h>

#include <sqlite3.h>

// Default Agent IDs.
#ifndef AGENT_ID_SQLITE_DTN
#define AGENT_ID_SQLITE_DTN "sqlite"
#endif // AGENT_ID_SQLITE_DTN
#ifndef AGENT_ID_SQLITE_IPN
#define AGENT_ID_SQLITE_IPN "9003"
#endif // AGENT_ID_SQLITE_IPN

struct sqlite_agent_params {
	char *sink_identifier;
	agent_no_t registered_agent_no;
	sqlite3 *db;
	QueueIdentifier_t cla_queue;
};

enum ud3tn_result sqlite_agent_setup(
	const struct bundle_agent_interface *const bai,
	struct sqlite_agent_params *params
);

#endif // SQLITE_AGENT_H_INCLUDED
