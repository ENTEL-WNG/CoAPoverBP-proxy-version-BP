// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef CLA_SQLITE_H
#define CLA_SQLITE_H

#include "agents/storage/storage_agent.pb.h"

#include "cla/cla.h"

#include "ud3tn/bundle_processor.h"
#include "ud3tn/result.h"

#include <stddef.h>

#include <sqlite3.h>

// Length of the SQLiteAgent queue.
#ifndef CLA_SQLITE_AGENT_QUEUE_LENGTH
#define CLA_SQLITE_AGENT_QUEUE_LENGTH 10
#endif // CLA_SQLITE_AGENT_QUEUE_LENGTH

// Set a SQLite busy timeout in milliseconds. Set to zero to turn off.
// See: https://www.sqlite.org/c3ref/busy_timeout.html
// Especially under load and with concurrent access to the storage agent it is
// necessary to use a timeout to prevent SQLITE_BUSY errors.
// See also: https://www.sqlite.org/src/doc/204dbc15a682125c/doc/wal-lock.md
#ifndef CLA_SQLITE_BUSY_TIMEOUT_MS
#define CLA_SQLITE_BUSY_TIMEOUT_MS 100
#endif // CLA_SQLITE_BUSY_TIMEOUT

// The node ID that the SQLite CLA will register (create a FIB entry) for.
#ifndef CLA_SQLITE_NODE_ID
#define CLA_SQLITE_NODE_ID "dtn:storage"
#endif // CLA_SQLITE_NODE_ID

enum cla_sqlite_command_type {
	SQLITE_COMMAND_UNDEFINED,    /* 0x00 */
	SQLITE_COMMAND_STORAGE_CALL, /* 0x01 */
	SQLITE_COMMAND_FINALIZE,     /* 0x02 */
};

struct cla_sqlite_command {
	enum cla_sqlite_command_type type;
	StorageCall storage_call;
};

const char *sqlite_name_get(void);

struct cla_config *sqlite_create(
	const char *const options[],
	const size_t option_count,
	const struct bundle_agent_interface *bundle_agent_interface
);

#endif /* CLA_SQLITE_H */
