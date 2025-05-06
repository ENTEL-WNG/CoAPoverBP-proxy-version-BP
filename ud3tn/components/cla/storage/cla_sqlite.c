// SPDX-License-Identifier: AGPL-3.0-or-later
#include "agents/sqlite_agent.h"
#include "agents/storage/storage_agent.pb.h"

#include "cla/cla.h"
#include "cla/cla_contact_tx_task.h"
#include "cla/storage/cla_sqlite.h"

#include "platform/hal_io.h"
#include "platform/hal_queue.h"
#include "platform/hal_semaphore.h"
#include "platform/hal_task.h"
#include "platform/hal_types.h"
#include "platform/posix/hal_types.h"

#include "ud3tn/eid.h"
#include "ud3tn/fib.h"
#include "ud3tn/result.h"
#include "ud3tn/simplehtab.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include <pb_decode.h>

#include <sqlite3.h>

static const char *CLA_NAME = "sqlite";
static const char *CLA_ADDR = "sqlite:";

static const char *SQL_CREATE_TABLE = (
	"CREATE TABLE IF NOT EXISTS bundles(source TEXT NOT NULL, destination TEXT, creation_timestamp INTEGER NOT NULL, sequence_number INTEGER NOT NULL, fragment_offset INTEGER NOT NULL, payload_length INTEGER NOT NULL, bundle BLOB, PRIMARY KEY(source, creation_timestamp, sequence_number, fragment_offset, payload_length));"
);

static const char *SQL_INSERT_INTO = (
	"INSERT INTO bundles(source, destination, creation_timestamp, sequence_number, fragment_offset, payload_length, bundle) VALUES (?, ?, ?, ?, ?, ?, ?);"
);

const char *SQL_SELECT_BY_PK = (
	"SELECT rowid from bundles WHERE source=? AND creation_timestamp=? AND sequence_number=? AND fragment_offset=? AND payload_length=?;"
);

const char *SQL_SELECT_BY_DESTINATION = "SELECT rowid from bundles WHERE destination GLOB ?";

struct sqlite_bundle_blob {
	sqlite3_blob *blob;
	sqlite3_int64 size;
	sqlite3_int64 offset;
};


static enum ud3tn_result sqlite_bundle_blob_open(
	sqlite3 *db,
	sqlite3_int64 row_id,
	int flags,
	struct sqlite_bundle_blob *bundle_blob)
{
	if (sqlite3_blob_open(
		db,
		"main",
		"bundles",
		"bundle",
		row_id,
		flags,
		&bundle_blob->blob
	) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to open BLOB: %s", sqlite3_errmsg(db));
		return UD3TN_FAIL;
	}

	bundle_blob->size = sqlite3_blob_bytes(bundle_blob->blob);
	bundle_blob->offset = 0;

	return UD3TN_OK;
}

static void sqlite_bundle_blob_reset(struct sqlite_bundle_blob *bundle_blob)
{
	bundle_blob->blob = NULL;
	bundle_blob->size = 0;
	bundle_blob->offset = 0;
}

static enum ud3tn_result sqlite_bundle_blob_close(
	sqlite3 *db,
	struct sqlite_bundle_blob *bundle_blob)
{
	int32_t res = sqlite3_blob_close(bundle_blob->blob);

	if (res != SQLITE_OK)
		LOGF_ERROR("SQLiteCLA: Failed to close BLOB: %s", sqlite3_errmsg(db));

	sqlite_bundle_blob_reset(bundle_blob);

	return res != SQLITE_OK ? UD3TN_FAIL : UD3TN_OK;
}

struct sqlite_link {
	struct cla_link base;
	sqlite3 *rx_db;
	sqlite3 *tx_db;
	QueueIdentifier_t agent_queue;
	struct sqlite_bundle_blob read_bundle_blob;
	struct sqlite_bundle_blob send_bundle_blob;
	sqlite3_stmt *select_stmt;
};

struct sqlite_config {
	struct cla_config base;
	struct sqlite_link *link;
	struct sqlite_agent_params *agent;
	pthread_t connection_task;
	char *filename;
};

const char *sqlite_name_get(void)
{
	return CLA_NAME;
}


static enum ud3tn_result sqlite_launch(struct cla_config *const config)
{
	struct sqlite_config *const sqlite_config = (
		(struct sqlite_config *)config
	);

	struct fib_request *req = malloc(
		sizeof(struct fib_request)
	);

	QueueIdentifier_t agent_queue = NULL;
	sqlite3 *rx_db = NULL;
	sqlite3 *tx_db = NULL;
	sqlite3 *agent_db = NULL;
	struct sqlite_agent_params *agent_params = NULL;

	if (!req)
		return UD3TN_FAIL;

	req->type = FIB_REQUEST_CREATE_LINK;
	req->flags = FIB_ENTRY_FLAG_DIRECT;
	req->node_id = NULL; // set to NULL if fails
	req->cla_addr = strdup(CLA_ADDR);
	if (!req->cla_addr)
		goto failed;
	req->node_id = strdup(CLA_SQLITE_NODE_ID);
	if (!req->node_id)
		goto failed;

	// Configure the CLA link
	bundle_processor_inform(
		config->bundle_agent_interface->bundle_signaling_queue,
		(struct bundle_processor_signal){
			.type = BP_SIGNAL_FIB_UPDATE_REQUEST,
			.fib_request = req,
		}
	);

	req = NULL; // Taken over by BP

	char *const filename = sqlite_config->filename;

	// Create a message queue to connect the sqlite agent with the CLA
	agent_queue = hal_queue_create(
		CLA_SQLITE_AGENT_QUEUE_LENGTH,
		sizeof(struct cla_sqlite_command)
	);
	if (!agent_queue) {
		LOG_ERROR("SQLiteCLA: Failed to create agent queue");
		goto failed;
	}

	if (sqlite3_config(SQLITE_CONFIG_MULTITHREAD) != SQLITE_OK) {
		LOG_ERROR(
			"SQLiteCLA: Failed to set SQLITE_CONFIG_SERIALIZED (SQLite needs to be compiled with the SQLITE_THREADSAFE=1 compile-time option)"
		);
		goto failed;
	}
	// Enable support for URI filenames to use in-memory databases and shared cache
	if (sqlite3_config(SQLITE_CONFIG_URI, 1) != SQLITE_OK) {
		LOG_ERROR("SQLiteCLA: Failed to set SQLITE_CONFIG_URI to enable URI filename support");
		goto failed;
	}

	LOGF_DEBUG("SQLiteCLA: Using database file: %s", filename);

	// Open database connections and create the tables
	if (sqlite3_open(filename, &tx_db) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to open database file: %s", sqlite3_errmsg(tx_db));
		goto failed;
	}
	if (sqlite3_open_v2(filename, &rx_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to open database file: %s", sqlite3_errmsg(rx_db));
		goto failed;
	}
	if (sqlite3_open(filename, &agent_db) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to open database file: %s", sqlite3_errmsg(agent_db));
		goto failed;
	}

	if (sqlite3_exec(tx_db, SQL_CREATE_TABLE, 0, 0, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to create table: %s", sqlite3_errmsg(tx_db));
		goto failed;
	}

	// Enable WAL jounral mode to prevent SQLITE_BUSY
	// https://www.sqlite.org/wal.html
	// https://www.sqlite.org/src/doc/204dbc15a682125c/doc/wal-lock.md
	if (sqlite3_exec(tx_db, "PRAGMA journal_mode=WAL;", 0, 0, NULL) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to enable WAL journal mode: %s",
			sqlite3_errmsg(tx_db)
		);
		goto failed;
	}
	sqlite3_busy_timeout(rx_db, CLA_SQLITE_BUSY_TIMEOUT_MS);
	sqlite3_busy_timeout(tx_db, CLA_SQLITE_BUSY_TIMEOUT_MS);
	sqlite3_busy_timeout(agent_db, CLA_SQLITE_BUSY_TIMEOUT_MS);

	agent_params = malloc(sizeof(struct sqlite_agent_params));
	if (!agent_params) {
		LOG_ERROR("SQLiteCLA: Failed to allocate sqlite_agent_params");
		goto failed;
	}
	agent_params->sink_identifier =
		get_eid_scheme(config->bundle_agent_interface->local_eid) == EID_SCHEME_IPN
			? AGENT_ID_SQLITE_IPN
			: AGENT_ID_SQLITE_DTN;
	agent_params->db = agent_db;
	agent_params->cla_queue = agent_queue;

	// Initialize the SQLite agent
	if (sqlite_agent_setup(config->bundle_agent_interface, agent_params) != 0) {
		LOG_ERROR("SQLiteCLA: Failed to initialize SQLiteAgent");
		// NOTE: `sqlite_agent_setup` has called `free()` on `agent_params` at this point.
		goto failed;
	}

	sqlite_config->agent = agent_params;

	// Initialize the link configuration
	sqlite_config->link = malloc(sizeof(struct sqlite_link));
	if (!sqlite_config->link) {
		LOG_ERROR("SQLiteCLA: Failed to allocate sqlite_link");
		goto failed;
	}
	sqlite_config->link->rx_db = rx_db;
	sqlite_config->link->tx_db = tx_db;
	sqlite_config->link->agent_queue = agent_queue;
	sqlite_config->link->select_stmt = NULL;
	sqlite_bundle_blob_reset(&sqlite_config->link->read_bundle_blob);
	sqlite_bundle_blob_reset(&sqlite_config->link->send_bundle_blob);

	if (cla_link_init(&sqlite_config->link->base, &sqlite_config->base,
			  NULL, true, true) != UD3TN_OK) {
		LOG_ERROR("SQLiteCLA: Failed to initialize sqlite_link");
		goto failed;
	}

	return UD3TN_OK;

failed:
	free(sqlite_config->link);
	free(agent_params);
	sqlite3_close(rx_db);
	sqlite3_close(tx_db);
	hal_queue_delete(agent_queue);
	if (req) {
		free(req->node_id);
		free(req->cla_addr);
		free(req);
	}

	LOG_ERROR("SQLiteCLA: Failed to configure CLA link");
	return UD3TN_FAIL;
}

static void sqlite_disconnect_handler(struct cla_link *link)
{
	// UNUSED
	(void)link;
}

static enum ud3tn_result sqlite_terminate(struct cla_config *config)
{
	struct sqlite_config *sqlite_config = (struct sqlite_config *)config;

	if (sqlite_config->link == NULL)
		goto term_no_link;

	// Deregister SQLiteAgent
	bundle_processor_perform_agent_action(
		config->bundle_agent_interface->bundle_signaling_queue,
		BP_SIGNAL_AGENT_DEREGISTER,
		(struct agent){
			.agent_no = sqlite_config->agent->registered_agent_no,
		}
	);

	// Signal the termination of RX/TX task
	cla_generic_disconnect_handler((struct cla_link *)sqlite_config->link);

	// Signal the termination of sqlite_read()
	struct cla_sqlite_command cmd = { .type = SQLITE_COMMAND_FINALIZE };

	hal_queue_push_to_back(sqlite_config->link->agent_queue, &cmd);

	// Close database connection
	if (sqlite3_close(sqlite_config->link->rx_db) != SQLITE_OK)
		LOG_INFO("SQLiteCLA: Error terminating RX DB connection!");
	if (sqlite3_close(sqlite_config->link->tx_db) != SQLITE_OK)
		LOG_INFO("SQLiteCLA: Error terminating TX DB connection!");
	if (sqlite3_close(sqlite_config->agent->db) != SQLITE_OK)
		LOG_INFO("SQLiteCLA: Error terminating agent DB connection!");

	// Cleanup allocated resources
	cla_link_wait_cleanup(&sqlite_config->link->base);

	hal_queue_delete(sqlite_config->link->agent_queue);

	free(sqlite_config->link);
	free(sqlite_config->agent);

term_no_link:
	free(sqlite_config->filename);
	free(sqlite_config);

	LOG_INFO("SQLiteCLA: Terminated gracefully");

	return UD3TN_OK;
}

static size_t sqlite_mbs_get(struct cla_config *const config)
{
	// UNUSED
	(void)config;

	return 0;
}

static struct cla_tx_queue sqlite_get_tx_queue(
	struct cla_config *config,
	const char *eid,
	const char *cla_addr)
{
	// UNUSED
	(void)eid;
	(void)cla_addr;

	struct sqlite_link *const link = ((struct sqlite_config *)config)->link;

	// No active link!
	if (!link)
		return (struct cla_tx_queue){ NULL, NULL };

	hal_semaphore_take_blocking(link->base.tx_queue_sem);

	// Freed while trying to obtain it
	if (!link->base.tx_queue_handle)
		return (struct cla_tx_queue){ NULL, NULL };

	return (struct cla_tx_queue){
		.tx_queue_handle = link->base.tx_queue_handle,
		.tx_queue_sem = link->base.tx_queue_sem,
	};
}

static enum cla_link_update_result sqlite_start_scheduled_contact(
	struct cla_config *config,
	const char *eid,
	const char *cla_addr)
{
	// UNUSED
	(void)eid;
	(void)cla_addr;
	(void)config;

	return CLA_LINK_UPDATE_PERFORMED;
}

static enum cla_link_update_result sqlite_end_scheduled_contact(
	struct cla_config *config,
	const char *eid,
	const char *cla_addr)
{
	// UNUSED
	(void)eid;
	(void)cla_addr;
	(void)config;

	return CLA_LINK_UPDATE_PERFORMED;
}

static enum cla_begin_packet_result sqlite_begin_packet(
	struct cla_link *link,
	const struct bundle *const bundle,
	size_t length,
	char *cla_addr)
{
	// UNUSED
	(void)cla_addr;

	LOGF_DEBUG("SQLiteCLA: Storing bundle %p with destination %s", bundle, bundle->destination);

	enum cla_begin_packet_result rv = CLA_BEGIN_PACKET_FAIL;

	struct sqlite_link *sqlite_link = (struct sqlite_link *)link;
	sqlite3 *db = sqlite_link->tx_db;
	sqlite3_stmt *stmt;

	if (sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to BEGIN IMMEDIATE TRANSACTION: %s",
			sqlite3_errmsg(db)
		);
		return CLA_BEGIN_PACKET_FAIL;
	}

	if (sqlite3_prepare_v2(db, SQL_INSERT_INTO, -1, &stmt, 0) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to prepare INSERT statement: %s",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	if (sqlite3_bind_text(stmt, 1, bundle->source, strlen(bundle->source), NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to bind source column: %s", sqlite3_errmsg(db));
		goto rollback;
	}

	if (sqlite3_bind_text(
		stmt, 2, bundle->destination, strlen(bundle->destination), NULL
	) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to bind destination column: %s", sqlite3_errmsg(db));
		goto rollback;
	}

	if (sqlite3_bind_int64(stmt, 3, bundle->creation_timestamp_ms) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to bind creation_timestamp column: %s",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	if (sqlite3_bind_int64(stmt, 4, bundle->sequence_number) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to bind sequence_number column: %s",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	if (sqlite3_bind_int64(stmt, 5, bundle->fragment_offset) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to bind fragment_offset column: %s",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	if (sqlite3_bind_int64(stmt, 6, bundle->payload_block->length) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to bind payload_length column: %s",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	if (sqlite3_bind_zeroblob(stmt, 7, length) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to bind bundle column: %s", sqlite3_errmsg(db));
		goto rollback;
	}

	const int exec_result = sqlite3_step(stmt);

	if (exec_result != SQLITE_DONE) {
		if (exec_result == SQLITE_CONSTRAINT) {
			LOGF_DEBUG(
				"SQLiteCLA: Bundle %p exists in storage already",
				bundle
			);
			rv = CLA_BEGIN_PACKET_DONE;
		} else {
			LOGF_WARN(
				"SQLiteCLA: Error executing SQL statement for bundle %p",
				bundle
			);
		}
		goto rollback;
	}

	if (sqlite3_finalize(stmt) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to finalize INSERT statement: %s",
			sqlite3_errmsg(db)
		);
		goto rollback_nofinalize;
	}

	/* Open the bundle blob to write the data incrementally when the sqlite_send_packet_data
	 * function is called.
	 */
	if (sqlite_bundle_blob_open(
		db,
		sqlite3_last_insert_rowid(db),
		1,
		&sqlite_link->send_bundle_blob
	) != UD3TN_OK) {
		LOGF_WARN(
			"SQLiteCLA: Failed opening blob for writing bundle %p",
			bundle
		);
		goto rollback_nofinalize;
	}

	rv = CLA_BEGIN_PACKET_OK;
	return rv;

rollback:
	sqlite3_finalize(stmt);
rollback_nofinalize:
	sqlite_bundle_blob_reset(&sqlite_link->send_bundle_blob);
	if (sqlite3_exec(db, "ROLLBACK TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK)
		LOGF_ERROR("SQLiteCLA: Failed to ROLLBACK TRANSACTION: %s", sqlite3_errmsg(db));

	return rv;
}

static enum ud3tn_result sqlite_send_packet_data(
	struct cla_link *link,
	const void *data,
	const size_t length)
{
	struct sqlite_link *sqlite_link = (struct sqlite_link *)link;
	sqlite3 *db = sqlite_link->tx_db;
	struct sqlite_bundle_blob *send_bundle_blob = &sqlite_link->send_bundle_blob;

	if (sqlite3_blob_write(
		send_bundle_blob->blob,
		data,
		length,
		send_bundle_blob->offset
	) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to write BLOB: %s", sqlite3_errmsg(db));
		sqlite_bundle_blob_reset(&sqlite_link->send_bundle_blob);
		if (sqlite3_exec(db, "ROLLBACK TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK)
			LOGF_ERROR(
				"SQLiteCLA: Failed to ROLLBACK TRANSACTION: %s",
				sqlite3_errmsg(db)
			);

		return UD3TN_FAIL;
	}

	send_bundle_blob->offset += length;

	return UD3TN_OK;
}

static enum ud3tn_result sqlite_end_packet(struct cla_link *link)
{
	struct sqlite_link *sqlite_link = (struct sqlite_link *)link;
	sqlite3 *db = sqlite_link->tx_db;

	if (sqlite_bundle_blob_close(db, &sqlite_link->send_bundle_blob) != UD3TN_OK) {
		if (sqlite3_exec(db, "ROLLBACK TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK)
			LOGF_ERROR(
				"SQLiteCLA: Failed to ROLLBACK TRANSACTION: %s",
				sqlite3_errmsg(db)
			);
		return UD3TN_FAIL;
	}

	if (sqlite3_exec(db, "END TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to END TRANSACTION: %s", sqlite3_errmsg(db));
		return UD3TN_FAIL;
	}

	return UD3TN_OK;
}

static void sqlite_reset_parsers(struct cla_link *const link)
{
	struct sqlite_link *sqlite_link = (struct sqlite_link *)link;

	if (sqlite_link->read_bundle_blob.blob)
		sqlite_bundle_blob_close(sqlite_link->rx_db, &sqlite_link->read_bundle_blob);

	rx_task_reset_parsers(&link->rx_task_data);
	link->rx_task_data.cur_parser = link->rx_task_data.bundle7_parser.basedata;
}

static size_t sqlite_forward_to_specific_parser(
	struct cla_link *const link,
	const uint8_t *const buffer,
	const size_t length)
{
	struct rx_task_data *const rx_data = &link->rx_task_data;
	size_t result = 0;

	switch (rx_data->payload_type) {
	case PAYLOAD_UNKNOWN:
		result = select_bundle_parser_version(rx_data, buffer, length);
		if (result == 0)
			sqlite_reset_parsers(link);
		break;
	case PAYLOAD_BUNDLE6:
		rx_data->cur_parser = rx_data->bundle6_parser.basedata;
		result = bundle6_parser_read(
			&rx_data->bundle6_parser,
			buffer,
			length
		);
		break;
	case PAYLOAD_BUNDLE7:
		rx_data->cur_parser = rx_data->bundle7_parser.basedata;
		result = bundle7_parser_read(
			&rx_data->bundle7_parser,
			buffer,
			length
		);
		break;
	default:
		sqlite_reset_parsers(link);
		return 0;
	}

	return result;
}

static enum ud3tn_result prepare_id_filter_statement(
	struct sqlite_link *link,
	struct cla_sqlite_command *cmd)
{
	sqlite3 *db = link->rx_db;
	sqlite3_stmt *stmt = link->select_stmt;
	CompoundBundleId id = cmd->storage_call.bundle_filter.id;

	if (!id.source_eid) {
		LOG_ERROR("SQLiteCLA: id.source_eid is NULL");
		return UD3TN_FAIL;
	}

	if (sqlite3_prepare_v2(db, SQL_SELECT_BY_PK, -1, &stmt, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to prepare SQL statement: %s", sqlite3_errmsg(db));
		return UD3TN_FAIL;
	}

	if (sqlite3_bind_text(
		stmt, 1, id.source_eid, strlen(id.source_eid), SQLITE_TRANSIENT
	) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to bind source column: %s",
			sqlite3_errmsg(db)
		);
		sqlite3_finalize(stmt);
		return UD3TN_FAIL;
	}

	if (sqlite3_bind_int64(stmt, 2, id.creation_timestamp) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to bind creation_timestamp column: %llu",
			sqlite3_errmsg(db)
		);
		sqlite3_finalize(stmt);
		return UD3TN_FAIL;
	}

	if (sqlite3_bind_int64(stmt, 3, id.sequence_number) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to bind sequence_number column: %llu",
			sqlite3_errmsg(db)
		);
		sqlite3_finalize(stmt);
		return UD3TN_FAIL;
	}

	if (sqlite3_bind_int64(stmt, 4, id.fragment_offset) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to bind fragment_offset column: %llu",
			sqlite3_errmsg(db)
		);
		sqlite3_finalize(stmt);
		return UD3TN_FAIL;
	}

	if (sqlite3_bind_int64(stmt, 5, id.payload_length) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to bind payload_length column: %llu",
			sqlite3_errmsg(db)
		);
		sqlite3_finalize(stmt);
		return UD3TN_FAIL;
	}

	link->select_stmt = stmt;

	return UD3TN_OK;
}

static enum ud3tn_result prepare_metadata_filter_statement(
	struct sqlite_link *link,
	struct cla_sqlite_command *cmd)
{
	sqlite3 *db = link->rx_db;
	sqlite3_stmt *stmt = link->select_stmt;
	BundleMetadataFilter metadata = cmd->storage_call.bundle_filter.metadata;

	if (!metadata.eid_glob) {
		LOG_ERROR("SQLiteCLA: metadata.eid_glob is NULL");
		return UD3TN_FAIL;
	}

	const char *eid_glob = metadata.eid_glob;

	if (sqlite3_prepare_v2(db, SQL_SELECT_BY_DESTINATION, -1, &stmt, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to prepare SQL statement: %s", sqlite3_errmsg(db));
		return UD3TN_FAIL;
	}

	// "The SQLITE_TRANSIENT value means that the content will likely change in the near future
	// and that SQLite should make its own private copy of the content before returning."
	// https://www.sqlite.org/c3ref/c_static.html
	if (sqlite3_bind_text(
		stmt, 1, eid_glob, strlen(eid_glob), SQLITE_TRANSIENT
	) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to bind destination column: %s",
			sqlite3_errmsg(db)
		);
		sqlite3_finalize(stmt);
		return UD3TN_FAIL;
	}

	link->select_stmt = stmt;

	return UD3TN_OK;
}

static enum ud3tn_result rollback_select_transaction(struct sqlite_link *link)
{
	LOG_WARN("SQLiteCLA: ROLLBACK TRANSACTION");

	sqlite3 *db = link->rx_db;

	if (sqlite3_exec(db, "ROLLBACK TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to ROLLBACK TRANSACTION: %s", sqlite3_errmsg(db));
		return UD3TN_FAIL;
	}

	link->select_stmt = NULL;

	return UD3TN_OK;
}

static enum ud3tn_result begin_select_transation(
	struct sqlite_link *link,
	struct cla_sqlite_command *cmd)
{
	sqlite3 *db = link->rx_db;

	if (sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to BEGIN TRANSACTION: %s", sqlite3_errmsg(db));
		return UD3TN_FAIL;
	}

	switch (cmd->storage_call.which_bundle_filter) {
	case StorageCall_id_tag:
		if (prepare_id_filter_statement(link, cmd) != UD3TN_OK) {
			rollback_select_transaction(link);
			return UD3TN_FAIL;
		}
		break;
	case StorageCall_metadata_tag:
		if (prepare_metadata_filter_statement(link, cmd) != UD3TN_OK) {
			rollback_select_transaction(link);
			return UD3TN_FAIL;
		}
		break;
	default:
		LOG_DEBUG("SQLiteCLA: Invalid bundle_filter option");
		rollback_select_transaction(link);
		return UD3TN_FAIL;
	};

	return UD3TN_OK;
}

static enum ud3tn_result end_select_transaction(
	struct sqlite_link *link)
{
	sqlite3 *db = link->rx_db;

	if (sqlite3_finalize(link->select_stmt) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to finalize SELECT statement: %s",
			sqlite3_errmsg(db)
		);
		rollback_select_transaction(link);
		link->select_stmt = NULL;
		return UD3TN_FAIL;
	}

	if (sqlite3_exec(db, "END TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteCLA: Failed to END TRANSACTION: %s", sqlite3_errmsg(db));
		link->select_stmt = NULL;
		return UD3TN_FAIL;
	}

	link->select_stmt = NULL;

	return UD3TN_OK;
}

enum agent_queue_receive_result {
	AGENT_QUEUE_RECEIVE_FAIL = -1,
	AGENT_QUEUE_RECEIVE_OK = 0,
	AGENT_QUEUE_RECEIVE_TERMINATE = 1,
};

static enum agent_queue_receive_result agent_queue_receive(
	struct sqlite_link *link,
	struct cla_sqlite_command *cmd)
{
	if (hal_queue_receive(link->agent_queue, cmd, -1) != UD3TN_OK) {
		LOG_ERROR("SQLiteCLA: Failed to receive bundle from agent_queue");
		return AGENT_QUEUE_RECEIVE_FAIL;
	}

	switch (cmd->type) {
	case SQLITE_COMMAND_STORAGE_CALL:
		LOG_DEBUG("SQLiteCLA: Received SQLITE_COMMAND_STORAGE_CALL");

		StorageOperation op = cmd->storage_call.operation;

		if (op != StorageOperation_STORAGE_OPERATION_PUSH_BUNDLES) {
			LOG_ERROR("SQLiteCLA: Only PUSH BUNDLE operations are handled.");
			return AGENT_QUEUE_RECEIVE_FAIL;
		}

		return AGENT_QUEUE_RECEIVE_OK;

	case SQLITE_COMMAND_FINALIZE:
		LOG_DEBUG("SQLiteCLA: Received SQLITE_COMMAND_FINALIZE: Terminate");
		return AGENT_QUEUE_RECEIVE_TERMINATE;

	case SQLITE_COMMAND_UNDEFINED:
		LOG_DEBUG("SQLiteCLA: Received SQLITE_COMMAND_UNDEFINED");
		return AGENT_QUEUE_RECEIVE_FAIL;
	}

	return AGENT_QUEUE_RECEIVE_OK;
}


static enum ud3tn_result sqlite_read(
	struct cla_link *link,
	uint8_t *buffer,
	size_t length,
	size_t *bytes_read)
{
	struct sqlite_link *const sqlite_link = (struct sqlite_link *)link;
	sqlite3 *const db = sqlite_link->rx_db;
	struct sqlite_bundle_blob *const read_bundle_blob = (
		&sqlite_link->read_bundle_blob
	);

	// Check queue for new commands if no SELECT statement is in progress.
	if (!sqlite_link->select_stmt) {
		struct cla_sqlite_command cmd;
		const enum agent_queue_receive_result cmd_type = agent_queue_receive(
			sqlite_link,
			&cmd
		);

		// Early return on error or termination signal
		if (cmd_type == AGENT_QUEUE_RECEIVE_FAIL)
			return UD3TN_FAIL;
		if (cmd_type == AGENT_QUEUE_RECEIVE_TERMINATE)
			return UD3TN_OK;

		// BEGIN TRANSACTION and prepare SELECT statement
		const enum ud3tn_result res = begin_select_transation(
			sqlite_link,
			&cmd
		);

		// Free StorageCall
		pb_release(StorageCall_fields, &cmd.storage_call);

		// Early return if SQL BEGIN TRANSACTION failed
		if (res != UD3TN_OK)
			return res;
	}

	// If no read operation is in progress, the next row for the current statement is queried
	if (!read_bundle_blob->blob) {
		const int sqlite_step_result = sqlite3_step(
			sqlite_link->select_stmt
		);

		if (sqlite_step_result == SQLITE_DONE)
			// If there are no more rows to read, end transaction and return
			return end_select_transaction(sqlite_link);
		else if (sqlite_step_result != SQLITE_ROW) {
			LOGF_ERROR(
				"SQLiteCLA: Invalid step result (%d), terminating read",
				sqlite_step_result
			);
			sqlite3_finalize(sqlite_link->select_stmt);
			rollback_select_transaction(sqlite_link);
			return UD3TN_FAIL;
		}

		// Open the next Bundle BLOB of the ongoing SELECT statement
		const sqlite3_int64 row_id = sqlite3_column_int64(
			sqlite_link->select_stmt,
			0
		);

		if (sqlite_bundle_blob_open(
			db, row_id, 0, read_bundle_blob
		) != UD3TN_OK) {
			sqlite3_finalize(sqlite_link->select_stmt);
			rollback_select_transaction(sqlite_link);
			return UD3TN_FAIL;
		}
	}

	// Recalucate length, if the remaining bundle is smaller than the buffer.
	if (read_bundle_blob->offset + (sqlite3_int64)length > read_bundle_blob->size)
		length = read_bundle_blob->size - read_bundle_blob->offset;

	if (length > INT_MAX) {
		LOGF_ERROR(
			"SQLiteCLA: Amount of bytes to be read exceeds the maximum allowed by sqlite3_blob_read: %llu > %i",
			length, INT_MAX
		);
		sqlite_bundle_blob_close(db, read_bundle_blob);
		sqlite3_finalize(sqlite_link->select_stmt);
		rollback_select_transaction(sqlite_link);
		return UD3TN_FAIL;
	}

	// Read
	if (sqlite3_blob_read(
		read_bundle_blob->blob,
		buffer,
		(int)length,
		read_bundle_blob->offset
	) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteCLA: Failed to read Bundle BLOB: %s",
			sqlite3_errmsg(db)
		);
		sqlite_bundle_blob_close(db, read_bundle_blob);
		sqlite3_finalize(sqlite_link->select_stmt);
		rollback_select_transaction(sqlite_link);
		return UD3TN_FAIL;
	}

	// Update the offset
	read_bundle_blob->offset += length;

	// The bundle is completely read, reset the buffers to parse the next one.
	if (read_bundle_blob->offset >= read_bundle_blob->size)
		sqlite_bundle_blob_close(db, read_bundle_blob);

	if (bytes_read)
		*bytes_read = length;

	return UD3TN_OK;
}

const struct cla_vtable sqlite_vtable = {
	.cla_name_get = sqlite_name_get,

	.cla_launch = sqlite_launch,
	.cla_terminate = sqlite_terminate,

	.cla_mbs_get = sqlite_mbs_get,

	.cla_get_tx_queue = sqlite_get_tx_queue,
	.cla_start_scheduled_contact = sqlite_start_scheduled_contact,
	.cla_end_scheduled_contact = sqlite_end_scheduled_contact,

	.cla_begin_packet = sqlite_begin_packet,
	.cla_end_packet = sqlite_end_packet,
	.cla_send_packet_data = sqlite_send_packet_data,

	.cla_rx_task_reset_parsers = sqlite_reset_parsers,
	.cla_rx_task_forward_to_specific_parser =
		sqlite_forward_to_specific_parser,

	.cla_read = sqlite_read,

	.cla_disconnect_handler = sqlite_disconnect_handler,
};

static enum ud3tn_result sqlite_init(
	struct sqlite_config *config,
	const char *filename,
	const struct bundle_agent_interface *bundle_agent_interface)
{
	// Initialize the CLA base configuration
	if (cla_config_init(&config->base, bundle_agent_interface) != UD3TN_OK) {
		LOG_ERROR("SQLiteCLA: Failed to initialize CLA base config");
		return UD3TN_FAIL;
	}
	config->base.vtable = &sqlite_vtable;
	config->link = NULL;
	config->filename = strdup(filename);

	if (!config->filename)
		return UD3TN_FAIL;

	return UD3TN_OK;
}

struct cla_config *sqlite_create(
	const char *const options[],
	const size_t option_count,
	const struct bundle_agent_interface *bundle_agent_interface)
{
	LOGF_DEBUG("SQLiteCLA: Using library version %s.", sqlite3_libversion());

	if (option_count != 1) {
		LOG_ERROR("SQLiteCLA: Option has to be a file path.");
		return NULL;
	}

	struct sqlite_config *config = malloc(sizeof(struct sqlite_config));

	if (!config) {
		LOG_ERROR("SQLiteCLA: Memory allocation failed!");
		return NULL;
	}

	if (sqlite_init(config, options[0],
		bundle_agent_interface) != UD3TN_OK) {
		free(config);
		LOG_ERROR("SQLiteCLA: Initialization failed!");
		return NULL;
	}

	return &config->base;
}
