// SPDX-License-Identifier: AGPL-3.0-or-later
#include "agents/sqlite_agent.h"
#include "agents/storage/storage_agent.pb.h"

#include "cla/storage/cla_sqlite.h"

#include "platform/hal_io.h"
#include "platform/hal_queue.h"
#include "platform/posix/hal_types.h"
#include "platform/hal_task.h"

#include "ud3tn/bundle_processor.h"
#include "ud3tn/common.h"
#include "ud3tn/eid.h"
#include "ud3tn/result.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <sqlite3.h>

#include <pb_decode.h>

static enum ud3tn_result delete_bundles_by_id(sqlite3 *db, CompoundBundleId *id)
{
	sqlite3_stmt *stmt;
	const char *sql = (
		"DELETE FROM bundles WHERE source=? AND creation_timestamp=? AND sequence_number=? AND fragment_offset=? AND payload_length=?;"
	);

	if (!id->source_eid) {
		LOG_ERROR("SQLiteAgent: id.source_eid is NULL");
		return UD3TN_FAIL;
	}

	if (sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteAgent: Failed to BEGIN IMMEDIATE TRANSACTION: %s",
			sqlite3_errmsg(db)
		);
		return UD3TN_FAIL;
	}

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteAgent: Failed to prepare SQL statement: %s", sqlite3_errmsg(db));
		goto rollback;
	}

	if (sqlite3_bind_text(
		stmt, 1, id->source_eid, strlen(id->source_eid), SQLITE_TRANSIENT
	) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteAgent: Failed to bind source column: %s",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	if (sqlite3_bind_int64(stmt, 2, id->creation_timestamp) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteAgent: Failed to bind creation_timestamp column: %llu",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	if (sqlite3_bind_int64(stmt, 3, id->sequence_number) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteAgent: Failed to bind sequence_number column: %llu",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	if (sqlite3_bind_int64(stmt, 4, id->fragment_offset) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteAgent: Failed to bind fragment_offset column: %llu",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	if (sqlite3_bind_int64(stmt, 5, id->payload_length) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteAgent: Failed to bind payload_length column: %llu",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	// Retry to execute the delete statement
	if (sqlite3_step(stmt) != SQLITE_DONE)
		goto rollback;

	if (sqlite3_finalize(stmt) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteAgent: Failed to finalize delete statement: %s",
			sqlite3_errmsg(db)
		);
		goto rollback_nofinalize;
	}

	if (sqlite3_exec(db, "END TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteAgent: Failed to END TRANSACTION: %s", sqlite3_errmsg(db));
		goto rollback_nofinalize;
	}

	return UD3TN_OK;

rollback:
	sqlite3_finalize(stmt);
rollback_nofinalize:
	if (sqlite3_exec(db, "ROLLBACK TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK)
		LOGF_ERROR("SQLiteAgent: Failed to ROLLBACK TRANSACTION: %s", sqlite3_errmsg(db));

	return UD3TN_FAIL;
}

static enum ud3tn_result delete_bundles_by_metadata(
	sqlite3 *db, const BundleMetadataFilter *metadata)
{
	sqlite3_stmt *stmt;
	const char *sql = "DELETE FROM bundles WHERE destination GLOB ?";
	char *eid_glob = metadata->eid_glob;

	if (!eid_glob) {
		LOG_ERROR("SQLiteAgent: metadata.eid_glob is NULL");
		return UD3TN_FAIL;
	}


	if (sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteAgent: Failed to BEGIN IMMEDIATE TRANSACTION: %s",
			sqlite3_errmsg(db)
		);
		return UD3TN_FAIL;
	}

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteAgent: Failed to prepare SQL statement: %s", sqlite3_errmsg(db));
		goto rollback;
	}

	if (sqlite3_bind_text(stmt, 1, eid_glob, strlen(eid_glob), NULL) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteAgent: Failed to bind destination column: %s",
			sqlite3_errmsg(db)
		);
		goto rollback;
	}

	// Retry to execute the delete statement
	if (sqlite3_step(stmt) != SQLITE_DONE)
		goto rollback;

	if (sqlite3_finalize(stmt) != SQLITE_OK) {
		LOGF_ERROR(
			"SQLiteAgent: Failed to finalize delete statement: %s",
			sqlite3_errmsg(db)
		);
		goto rollback_nofinalize;
	}

	if (sqlite3_exec(db, "END TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
		LOGF_ERROR("SQLiteAgent: Failed to END TRANSACTION: %s", sqlite3_errmsg(db));
		goto rollback_nofinalize;
	}

	return UD3TN_OK;

rollback:
	sqlite3_finalize(stmt);
rollback_nofinalize:
	if (sqlite3_exec(db, "ROLLBACK TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK)
		LOGF_ERROR("SQLiteAgent: Failed to ROLLBACK TRANSACTION: %s", sqlite3_errmsg(db));

	return UD3TN_FAIL;
}

static enum ud3tn_result storage_operation_delete_bundles(sqlite3 *db, StorageCall *cmd)
{
	switch (cmd->which_bundle_filter) {
	case StorageCall_id_tag:
		if (delete_bundles_by_id(db, &cmd->bundle_filter.id) != UD3TN_OK)
			return UD3TN_FAIL;

		break;
	case StorageCall_metadata_tag:
		if (delete_bundles_by_metadata(db, &cmd->bundle_filter.metadata) != UD3TN_OK)
			return UD3TN_FAIL;

		break;
	default:
		LOG_DEBUG("SQLiteAgent: Invalid bundle_filter option");
		return UD3TN_FAIL;
	};

	return UD3TN_OK;
}

static enum ud3tn_result storage_operation_push_bundles(
	sqlite3 *db,
	QueueIdentifier_t cla_queue,
	StorageCall *storage_call)
{
	(void)db;

	const struct cla_sqlite_command sqlite_cmd =  {
		.type = SQLITE_COMMAND_STORAGE_CALL,
		.storage_call = *storage_call,
	};

	if (hal_queue_try_push_to_back(cla_queue, &sqlite_cmd, 0) != UD3TN_OK) {
		LOG_ERROR("SQLiteAgent: Failed to push the StorageCall into the agent_queue");
		return UD3TN_FAIL;
	}

	return UD3TN_OK;
}

static enum ud3tn_result storage_operation_list_bundles(
	const sqlite3 *db,
	const StorageCall *cmd)
{
	(void)db;
	(void)cmd;

	LOG_WARN("SQLiteAgent: STORAGE_OPERATION_LIST_BUNDLES unimplemented!");
	return UD3TN_OK;
}


static enum ud3tn_result parse_storage_call(uint8_t *data, size_t length, StorageCall *storage_call)
{
	pb_istream_t stream = pb_istream_from_buffer(data, length);

	if (!pb_decode_ex(&stream, StorageCall_fields, storage_call, PB_DECODE_DELIMITED)) {
		LOGF_ERROR("SQLiteAgent: Protobuf decode error: %s", PB_GET_ERROR(&stream));
		return UD3TN_FAIL;
	}

	return UD3TN_OK;
}

static void sqlite_agent_callback(struct bundle_adu data, void *p, const void *bp_context)
{
	sqlite3 *db = ((struct sqlite_agent_params *)p)->db;
	QueueIdentifier_t cla_queue = ((struct sqlite_agent_params *)p)->cla_queue;
	StorageCall storage_call = StorageCall_init_default;

	if (!data.bdm_auth_validated) {
		LOGF_WARN(
			"SQLiteAgent: Incoming bundle ADU (from: \"%s\") not authorized",
			data.source
		);
		pb_release(StorageCall_fields, &storage_call);
		goto done;
	}

	parse_storage_call(data.payload, data.length, &storage_call);
	char *eid_glob = storage_call.bundle_filter.metadata.eid_glob;

	switch (storage_call.operation) {
	case StorageOperation_STORAGE_OPERATION_DELETE_BUNDLES:
		LOGF_DEBUG(
			"SQLiteAgent: Received STORAGE_OPERATION_DELETE_BUNDLES (filter.eid_glob = %s)",
			eid_glob
		);
		storage_operation_delete_bundles(db, &storage_call);
		pb_release(StorageCall_fields, &storage_call);
		break;
	case StorageOperation_STORAGE_OPERATION_PUSH_BUNDLES:
		LOGF_DEBUG(
			"SQLiteAgent: Received STORAGE_OPERATION_PUSH_BUNDLES (filter.eid_glob = %s)",
			eid_glob
		);
		storage_operation_push_bundles(db, cla_queue, &storage_call);
		// BundleFilter is freed in SQLiteCLA
		break;
	case StorageOperation_STORAGE_OPERATION_LIST_BUNDLES:
		LOGF_DEBUG(
			"SQLiteAgent: Received STORAGE_OPERATION_LIST_BUNDLES (filter.eid_glob = %s)",
			eid_glob
		);
		storage_operation_list_bundles(db, &storage_call);
		pb_release(StorageCall_fields, &storage_call);
		break;
	default:
		(void)eid_glob;
		LOG_WARN("SQLiteAgent: Unsupported StorageOperation");
		pb_release(StorageCall_fields, &storage_call);
		break;
	}

done:
	// Free allocated data
	bundle_adu_free_members(data);
}

enum ud3tn_result sqlite_agent_setup(
	const struct bundle_agent_interface *const bai,
	struct sqlite_agent_params *params)
{
	const struct agent agent = {
		.auth_trx = true,
		.is_subscriber = true,
		.sink_identifier = params->sink_identifier,
		.trx_callback = sqlite_agent_callback,
		.param = params,
	};

	agent_no_t agent_no = bundle_processor_perform_agent_action(
		bai->bundle_signaling_queue,
		BP_SIGNAL_AGENT_REGISTER,
		agent
	);

	if (agent_no == AGENT_NO_INVALID) {
		LOG_ERROR("SQLiteAgent: Failed to register with the bundle processor");
		free(params);
		return UD3TN_FAIL;
	}

	params->registered_agent_no = agent_no;

	return UD3TN_OK;
}
