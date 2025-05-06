// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

// Next version of the Application Agent Protocol, supporting forwarding modules
// and FIB control in extension to regular bundle reception/delivery.
// NOTE: This is still experimental - use with caution!

#include "aap2/aap2_agent.h"
#include "aap2/aap2.pb.h"

#include "bundle6/create.h"

#include "bundle7/create.h"

#include "cla/posix/cla_tcp_util.h"

#include "platform/hal_io.h"
#include "platform/hal_queue.h"
#include "platform/hal_semaphore.h"
#include "platform/hal_task.h"
#include "platform/hal_time.h"
#include "platform/hal_types.h"

#include "platform/posix/pipe_queue_util.h"
#include "platform/posix/socket_util.h"

#include "ud3tn/agent_manager.h"
#include "ud3tn/bundle.h"
#include "ud3tn/bundle_processor.h"
#include "ud3tn/common.h"
#include "ud3tn/eid.h"
#include "ud3tn/fib.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include <netinet/in.h>
#include <arpa/inet.h>

struct aap2_agent_config {
	const struct bundle_agent_interface *bundle_agent_interface;

	uint8_t bp_version;
	uint64_t lifetime_ms;
	uint64_t max_bundle_size_bytes;

	int listen_socket;

	TaskIdentifier_t listener_task;

	struct aap2_agent_connection_list {
		struct aap2_agent_comm_config *config;
		Semaphore_t agent_conn_mutex;
		struct aap2_agent_connection_list *next;
	} *conn_list;
	Semaphore_t conn_list_mutex;
	bool termination_flag; // indicates the listener itself cleans up
	Semaphore_t termination_flag_mutex;
};

struct aap2_agent_comm_config {
	struct aap2_agent_config *parent;
	int socket_fd;
	pb_istream_t pb_istream;

	QueueIdentifier_t agent_queue;

	bool is_configured;
	bool dispatch_auth;
	bool fib_auth;

	bool is_subscriber;
	char *registered_eid;
	agent_no_t registered_agent_no;
	char *secret;
	int keepalive_timeout_ms;

	Semaphore_t agent_conn_mutex;

	// TODO: Move to common multiplexer/demux state in BP. For now, allow
	// only one sender per ID.
	uint64_t last_bundle_timestamp_ms;
	uint64_t last_bundle_sequence_number;
};

struct dispatch_info {
	struct bundle *bundle;
	enum bundle_dispatch_reason reason;
	char *orig_node_id;
	char *orig_cla_addr;
};

enum aap2_agent_command_type {
	AAP2_ACMD_INVALID = 0,
	AAP2_ACMD_ADU,
	AAP2_ACMD_FIB,
	AAP2_ACMD_DISPATCH,
	AAP2_ACMD_FINALIZE,
};

struct fib_association {
	char *node_id;
	char *cla_addr;
	enum fib_entry_flags flags;
	enum fib_link_status status;
};

struct aap2_agent_command {
	enum aap2_agent_command_type type;
	union {
		struct bundle_adu bundle_adu;
		struct fib_association fib_assoc;
		struct dispatch_info disp_info;
	};
};

// Representation of AAP 2.0 BundleADUFlags repeated enum field.
struct bundle_adu_flags {
	bool bpdu;
	bool with_bdm_auth;
};

// forward declaration
static void aap2_agent_comm_task(void *const param);

static bool pb_recv_callback(pb_istream_t *stream, uint8_t *buf, size_t count)
{
	if (count == 0)
		return true;

	const int sock = (intptr_t)stream->state;
	ssize_t recvd = tcp_recv_all(sock, buf, count);

	// EOF?
	if (!recvd)
		stream->bytes_left = 0;
	else if (recvd < 0)
		LOG_ERRNO("AAP2Agent", "recv()", errno);

	return (size_t)recvd == count;
}

static int spawn_conn_task(struct aap2_agent_config *const config,
			   const int conn_fd)
{
	struct aap2_agent_comm_config *child_config = malloc(
		sizeof(struct aap2_agent_comm_config)
	);
	struct aap2_agent_connection_list *conn_entry = malloc(
		sizeof(struct aap2_agent_connection_list)
	);
	if (!child_config || !conn_entry) {
		LOG_ERROR("AAP2Agent: Error allocating memory for config!");
		goto fail_alloc;
	}

	child_config->parent = config;
	child_config->socket_fd = conn_fd;
	child_config->last_bundle_timestamp_ms = 0;
	child_config->last_bundle_sequence_number = 0;
	child_config->is_configured = false;
	child_config->dispatch_auth = false;
	child_config->fib_auth = false;
	child_config->is_subscriber = false;
	child_config->registered_eid = NULL;
	child_config->registered_agent_no = AGENT_NO_INVALID;
	child_config->secret = NULL;
	child_config->keepalive_timeout_ms = -1; // infinite

	child_config->pb_istream = (pb_istream_t){
		&pb_recv_callback,
		(void *)(intptr_t)conn_fd,
		SIZE_MAX,
		NULL,
	};

	child_config->agent_queue = hal_queue_create(
		AAP2_AGENT_QUEUE_LENGTH,
		sizeof(struct aap2_agent_command)
	);
	if (!child_config->agent_queue) {
		LOG_ERROR("AAP2Agent: Cannot create queue");
		goto fail_queue;
	}

	conn_entry->agent_conn_mutex = hal_semaphore_init_binary();
	conn_entry->next = NULL;

	if (!conn_entry->agent_conn_mutex) {
		LOG_ERROR("AAP2Agent: Cannot create mutex");
		goto fail_mutex;
	}

	child_config->agent_conn_mutex = conn_entry->agent_conn_mutex;
	conn_entry->config = child_config;

	const enum ud3tn_result task_creation_result = hal_task_create(
		aap2_agent_comm_task,
		child_config,
		true,
		NULL
	);

	if (task_creation_result != UD3TN_OK) {
		LOG_ERROR("AAP2Agent: Error starting comm. task!");
		goto fail_task;
	}

	// Prepend to list
	hal_semaphore_take_blocking(config->conn_list_mutex);
	conn_entry->next = config->conn_list;
	config->conn_list = conn_entry;
	hal_semaphore_release(config->conn_list_mutex);

	return 0;

fail_task:
	hal_semaphore_delete(conn_entry->agent_conn_mutex);
fail_mutex:
	hal_queue_delete(child_config->agent_queue);
fail_queue:
fail_alloc:
	free(child_config);
	free(conn_entry);
	return -1;
}

static void clean_up_comm_task_resources(
	struct aap2_agent_connection_list *const entry)
{
	hal_queue_delete(entry->config->agent_queue);
#ifndef __APPLE__
	// Done earlier on Apple systems.
	close(entry->config->socket_fd);
#endif // __APPLE__
	free(entry->config);
	hal_semaphore_delete(entry->agent_conn_mutex);
	free(entry);
}

static void aap2_agent_listener_task(void *const param)
{
	struct aap2_agent_config *const config = (
		(struct aap2_agent_config *)param
	);

	for (;;) {
		struct sockaddr_storage incoming;
		socklen_t incoming_addr_len = sizeof(struct sockaddr_storage);
		char addrstrbuf[sizeof(struct sockaddr_storage)];

		int conn_fd = accept(
			config->listen_socket,
			(struct sockaddr *)&incoming,
			&incoming_addr_len
		);

		if (conn_fd == -1) {
			if (errno == EINVAL)
				LOG_DEBUG("AAP2Agent: accept() reported invalid value, assuming socket was closed, requesting termination.");
			else
				LOG_ERRNO_WARN("AAP2Agent", "accept()", errno);
			break;
		}

		switch (incoming.ss_family) {
		case AF_UNIX:
			LOG_INFO("AAP2Agent: Accepted connection from UNIX Domain Socket.");
			break;
		case AF_INET: {
			struct sockaddr_in *in =
				(struct sockaddr_in *)&incoming;
			LOGF_INFO(
				"AAP2Agent: Accepted connection from '%s'.",
				inet_ntop(in->sin_family, &in->sin_addr,
					  addrstrbuf, sizeof(addrstrbuf))
			);
			break;
		}
		case AF_INET6: {
			struct sockaddr_in6 *in =
				(struct sockaddr_in6 *)&incoming;
			LOGF_INFO(
				"AAP2Agent: Accepted connection from '%s'.",
				inet_ntop(in->sin6_family, &in->sin6_addr,
					  addrstrbuf, sizeof(addrstrbuf))
			);
			break;
		}
		default:
			close(conn_fd);
			LOG_WARN("AAP2Agent: Unknown address family. Connection closed!");
			continue;
		}

		if (spawn_conn_task(config, conn_fd) < 0)
			close(conn_fd);
	}

	// Await all children to terminate.
	LOG_DEBUG("AAP2Agent: Terminating all children cleanly...");
	hal_semaphore_take_blocking(config->termination_flag_mutex);
	config->termination_flag = true;
	hal_semaphore_take_blocking(config->conn_list_mutex);
	hal_semaphore_release(config->termination_flag_mutex);
	while (config->conn_list) {
		struct aap2_agent_connection_list *cur = config->conn_list;

		if (cur->config->is_subscriber) {
			struct aap2_agent_command cmd = {
				.type = AAP2_ACMD_FINALIZE,
			};

			hal_queue_push_to_back(cur->config->agent_queue, &cmd);
		}
		shutdown(cur->config->socket_fd, SHUT_RDWR); // signal term
#ifdef __APPLE__
		// We have to force it on MacOS.
		close(cur->config->socket_fd);
#endif // __APPLE__
		hal_semaphore_take_blocking(cur->agent_conn_mutex);
		config->conn_list = cur->next;
		clean_up_comm_task_resources(cur);
	}
	hal_semaphore_take_blocking(config->termination_flag_mutex);
}

static bool write_callback(pb_ostream_t *stream, const uint8_t *buf, size_t count)
{
	struct tcp_write_to_socket_param *const wsp = stream->state;

	tcp_write_to_socket(wsp, buf, count);

	return !wsp->errno_;
}

static int send_message(const int socket_fd,
			const pb_msgdesc_t *const fields,
			const void *const src_struct)
{
	struct tcp_write_to_socket_param wsp = {
		.socket_fd = socket_fd,
		.errno_ = 0,
	};

	pb_ostream_t stream = {
		&write_callback,
		&wsp,
		SIZE_MAX,
		0,
		NULL,
	};

	const bool ret = pb_encode_ex(
		&stream,
		fields,
		src_struct,
		PB_ENCODE_DELIMITED
	);

	if (wsp.errno_) {
		LOG_ERRNO("AAP2Agent", "send()", wsp.errno_);
	} else if (!ret) {
		LOGF_ERROR(
			"AAP2Agent: Protobuf encode error: %s",
			PB_GET_ERROR(&stream)
		);
		wsp.errno_ = EIO;
	}

	return -wsp.errno_;
}

static void agent_msg_recv(struct bundle_adu data, void *param,
			   const void *bp_context)
{
	(void)bp_context;

	struct aap2_agent_comm_config *const config = (
		(struct aap2_agent_comm_config *)param
	);

	LOGF_DEBUG(
		"AAP2Agent: Got Bundle for EID \"%s\" from \"%s\", forwarding.",
		config->registered_eid,
		data.source
	);

	struct aap2_agent_command cmd = {
		.type = AAP2_ACMD_ADU,
		.bundle_adu = data,
	};

	hal_queue_push_to_back(config->agent_queue, &cmd);
}

static bool agent_dispatch(struct bundle *const bundle,
			   enum bundle_dispatch_reason reason,
			   const char *orig_node_id,
			   const char *orig_cla_addr,
			   void *param, const void *bp_context)
{
	(void)bp_context;

	struct aap2_agent_comm_config *const config = (
		(struct aap2_agent_comm_config *)param
	);

	LOGF_DEBUG("AAP2Agent: Got Bundle dispatch request for bundle %p with dst = \"%s\" and reason code %d, triggering BDM.",
		   bundle, bundle->destination, (int)reason);

	struct dispatch_info dispatch_info = {
		.bundle = bundle,
		.reason = reason,
		.orig_node_id = orig_node_id ? strdup(orig_node_id) : NULL,
		.orig_cla_addr = orig_cla_addr ? strdup(orig_cla_addr) : NULL,
	};

	struct aap2_agent_command cmd = {
		.type = AAP2_ACMD_DISPATCH,
		.disp_info = dispatch_info,
	};

#ifdef __clang_analyzer__
	// Silence false positive sending things through queue.
	free(dispatch_info.orig_node_id);
	free(dispatch_info.orig_cla_addr);
#endif

	hal_queue_push_to_back(config->agent_queue, &cmd);

	// NOTE that when this returns `true` the ownership of the bundle lies
	// within the AAP 2.0 agent comm. task!
	return true;
}

static void agent_fib_update(const struct fib_entry *const entry,
			     const char *node_id,
			     enum fib_link_status link_status,
			     void *param, const void *bp_context)
{
	(void)bp_context;

	struct aap2_agent_comm_config *const config = (
		(struct aap2_agent_comm_config *)param
	);

	LOGF_DEBUG("AAP2Agent: Got FIB update for CLA addr = \"%s\", forwarding.",
		   entry->cla_addr);

	struct fib_association entry_dup;

	entry_dup.node_id = node_id ? strdup(node_id) : NULL;
	entry_dup.cla_addr = strdup(entry->cla_addr);
	entry_dup.status = link_status;
	entry_dup.flags = entry->flags;

	ASSERT(entry_dup.cla_addr != NULL);
	if (!entry_dup.cla_addr) {
		free(entry_dup.node_id);
		return;
	}

	ASSERT(node_id == NULL || entry_dup.node_id != NULL);
	if (node_id != NULL && entry_dup.node_id == NULL) {
		free(entry_dup.cla_addr);
		return;
	}

	struct aap2_agent_command cmd = {
		.type = AAP2_ACMD_FIB,
		.fib_assoc = entry_dup,
	};

#ifdef __clang_analyzer__
	// Silence false positive sending things through queue.
	free(entry_dup.node_id);
	free(entry_dup.cla_addr);
#endif

	hal_queue_push_to_back(config->agent_queue, &cmd);
	// cppcheck-suppress memleak
}

static bool agent_liveliness_check(void *param)
{
	struct aap2_agent_comm_config *const config = param;

	const int poll_result = poll_recv_timeout(config->socket_fd, 0);

	// timeout -> connection assumed OK and nothing to receive
	if (poll_result == 0)
		return true;

	// ready-to-recv and RPC -> assume OK and give control back
	if (poll_result > 0 && !config->is_subscriber)
		return true;

	// ready-to-recv but subscribing client -> either connection closed or
	// invalid state and we can terminate anyway
	if (poll_result > 0 && config->is_subscriber) {
		uint8_t tmp;

		if (recv(config->socket_fd, &tmp, 1, 0) != 0)
			LOG_INFO("AAP2Agent: Unexpected data on socket!");

		(void)tmp;
	}

	struct aap2_agent_command cmd = {
		.type = AAP2_ACMD_FINALIZE,
	};

	// force finalization if there is a subscribing client
	if (config->is_subscriber)
		hal_queue_push_to_back(config->agent_queue, &cmd);

	return false;
}

static agent_no_t register_sink(const char *sink_identifier, bool is_subscriber,
			 bool auth_fib, bool auth_bdm,
			 const char *secret,
			 struct aap2_agent_comm_config *config)
{
	return bundle_processor_perform_agent_action(
		config->parent->bundle_agent_interface->bundle_signaling_queue,
		BP_SIGNAL_AGENT_REGISTER,
		(struct agent) {
			.auth_trx = true,
			.auth_fib = auth_fib,
			.auth_bdm = auth_bdm,
			.is_subscriber = is_subscriber,
			// NOTE that both sink_identifier and secret have to be
			// valid for the full duration of the registration!
			.sink_identifier = sink_identifier,
			.secret = secret,
			.trx_callback = is_subscriber ? agent_msg_recv : NULL,
			.fib_callback = (
				(is_subscriber && auth_fib)
				? agent_fib_update : NULL
			),
			.bdm_callback = (
				(is_subscriber && auth_bdm)
				? agent_dispatch : NULL
			),
			.is_alive_callback = agent_liveliness_check,
			.param = config,
		}
	);
}

static void deregister_sink(struct aap2_agent_comm_config *config)
{
	if (config->registered_eid == NULL)
		return;

	const char *agent_id = get_agent_id_ptr(
		config->registered_eid
	);

	LOGF_INFO(
		"AAP2Agent: De-registering agent ID \"%s\".",
		agent_id
	);

	// NOTE: This may return AGENT_NO_INVALID is the agent was auto-removed,
	// e.g. if the liveliness check failed.
	// NOTE 2: It also ensures that all references to the EID are removed
	// from AM before returning from the function, so we can safely free().
	bundle_processor_perform_agent_action(
		config->parent->bundle_agent_interface->bundle_signaling_queue,
		BP_SIGNAL_AGENT_DEREGISTER,
		(struct agent){
			.auth_trx = true,
			.auth_fib = config->fib_auth,
			.auth_bdm = config->dispatch_auth,
			.is_subscriber = config->is_subscriber,
			.sink_identifier = agent_id,
			.agent_no = config->registered_agent_no,
		}
	);

	free(config->registered_eid);
	config->registered_eid = NULL;
	free(config->secret);
	config->secret = NULL;
}

static uint64_t allocate_sequence_number(
	struct aap2_agent_comm_config *const config,
	const uint64_t time_ms)
{
	// If a previous bundle was sent less than one millisecond ago, we need
	// to increment the sequence number.
	if (config->last_bundle_timestamp_ms == time_ms)
		return ++config->last_bundle_sequence_number;

	config->last_bundle_timestamp_ms = time_ms;
	config->last_bundle_sequence_number = 1;

	return 1;
}

static aap2_ResponseStatus process_configure_msg(
	struct aap2_agent_comm_config *const config,
	aap2_ConnectionConfig *const msg)
{
	const bool auth_fib = (
		msg->auth_type == aap2_AuthType_AUTH_TYPE_FIB_CONTROL ||
		msg->auth_type == aap2_AuthType_AUTH_TYPE_FIB_AND_DISPATCH
	);
	const bool auth_bdm = (
		msg->auth_type == aap2_AuthType_AUTH_TYPE_BUNDLE_DISPATCH ||
		msg->auth_type == aap2_AuthType_AUTH_TYPE_FIB_AND_DISPATCH
	);

	LOGF_INFO(
		"AAP2Agent: Received request to %s for EID \"%s\"%s%s.",
		msg->is_subscriber ? "subscribe" : "register",
		msg->endpoint_id,
		auth_bdm ? " + dispatch bundles" : "",
		auth_fib ? " + FIB updates" : ""
	);

	// Clean up a previous registration in case there is one.
	deregister_sink(config);

	if (validate_eid(msg->endpoint_id) != UD3TN_OK) {
		LOGF_INFO(
			"AAP2Agent: Invalid EID provided: \"%s\"",
			msg->endpoint_id
		);
		return aap2_ResponseStatus_RESPONSE_STATUS_INVALID_REQUEST;
	}

	const char *sink_id = get_agent_id_ptr(
		msg->endpoint_id
	);

	if (!sink_id) {
		LOGF_WARN(
			"AAP2Agent: Cannot obtain sink for EID: \"%s\"",
			msg->endpoint_id
		);
		return aap2_ResponseStatus_RESPONSE_STATUS_INVALID_REQUEST;
	}

	const agent_no_t reg_agent_no = register_sink(
		sink_id,
		msg->is_subscriber,
		auth_fib,
		auth_bdm,
		msg->secret,
		config
	);

	if (reg_agent_no == AGENT_NO_INVALID) {
		LOG_INFO("AAP2Agent: Registration request declined.");
		return aap2_ResponseStatus_RESPONSE_STATUS_UNAUTHORIZED;
	}

	config->registered_eid = msg->endpoint_id;
	config->registered_agent_no = reg_agent_no;
	msg->endpoint_id = NULL; // take over freeing
	config->secret = msg->secret;
	msg->secret = NULL; // take over freeing

	config->keepalive_timeout_ms = -1; // infinite by default

	struct timespec recvtimeo_tv = {
		.tv_sec = 0,
		.tv_nsec = 0,
	};

	if (msg->keepalive_seconds != 0) {
		if (msg->keepalive_seconds >= (INT32_MAX / 1000 / 2)) {
			LOGF_WARN(
				"AAP2Agent: Keepalive timeout of %d sec is too large, ignoring.",
				msg->keepalive_seconds
			);
		} else {
			config->keepalive_timeout_ms = (
				msg->keepalive_seconds * 1000
			);
			// For active clients we wait for twice the specified
			// timeout until we terminate the connection.
			if (!msg->is_subscriber) {
				config->keepalive_timeout_ms *= 2;

				const int to = config->keepalive_timeout_ms;

				recvtimeo_tv.tv_sec = to / 1000;
				recvtimeo_tv.tv_nsec = (to % 1000) * 1000000;
			}
		}
	}

	setsockopt(
		config->socket_fd,
		SOL_SOCKET,
		SO_RCVTIMEO,
		(const char *)&recvtimeo_tv,
		sizeof(recvtimeo_tv)
	);

	config->is_configured = true;
	config->fib_auth = auth_fib;
	config->dispatch_auth = auth_bdm;

	if (msg->is_subscriber) {
		LOG_INFO("AAP2Agent: Switching control flow!");
		config->is_subscriber = true;
	}

	return aap2_ResponseStatus_RESPONSE_STATUS_SUCCESS;
}

static struct bundle_adu_flags get_adu_flags(
	const aap2_BundleADUFlags *flags_field, size_t flags_count)
{
	struct bundle_adu_flags rv = {};

	for (size_t i = 0; i < flags_count; i++) {
		switch (flags_field[i]) {
		case aap2_BundleADUFlags_BUNDLE_ADU_BPDU:
			rv.bpdu = true;
			break;
		case aap2_BundleADUFlags_BUNDLE_ADU_WITH_BDM_AUTH:
			rv.with_bdm_auth = true;
			break;
		default:
			break;
		}
	}

	return rv;
}

static aap2_ResponseStatus process_adu_msg(
	struct aap2_agent_comm_config *const config,
	aap2_BundleADU *const msg,
	uint8_t *payload_data,
	aap2_AAPResponse *response)
{
	struct bundle_adu_flags adu_flags = get_adu_flags(
		msg->adu_flags,
		msg->adu_flags_count
	);

	LOGF_DEBUG(
		"AAP2Agent: Received %s%s (l = %zu) for dst. EID \"%s\".",
		adu_flags.bpdu ? "BIBE BPDU" : "bundle",
		adu_flags.with_bdm_auth ? " with BDM auth." : "",
		msg->payload_length,
		msg->dst_eid
	);

	if (msg->creation_timestamp_ms != 0 || msg->sequence_number != 0) {
		LOG_WARN("AAP2Agent: User-defined creation timestamps are unsupported!");
		free(payload_data);
		return aap2_ResponseStatus_RESPONSE_STATUS_INVALID_REQUEST;
	}

	if (!config->registered_eid) {
		LOG_WARN("AAP2Agent: No agent ID registered, dropping!");
		free(payload_data);
		return aap2_ResponseStatus_RESPONSE_STATUS_NOT_FOUND;
	}

	if (!payload_data) {
		LOG_WARN("AAP2Agent: Cannot handle ADU without payload data!");
		return aap2_ResponseStatus_RESPONSE_STATUS_ERROR;
	}

	enum bundle_proc_flags flags = BUNDLE_FLAG_NONE;
	size_t payload_length = msg->payload_length;

	if (adu_flags.bpdu) {
		LOG_DEBUG("AAP2Agent: ADU is a BPDU, prepending AR header!");

		#ifdef BIBE_CL_DRAFT_1_COMPATIBILITY
			const uint8_t typecode = 7;
		#else
			const uint8_t typecode = 3;
		#endif

		const size_t ar_size = payload_length + 2;
		uint8_t *const ar_bytes = malloc(ar_size);

		memcpy(
			ar_bytes + 2,
			payload_data,
			payload_length
		);
		ar_bytes[0] = 0x82;     // CBOR array of length 2
		ar_bytes[1] = typecode; // Integer (record type)

		free(payload_data);
		payload_data = ar_bytes;
		payload_length = ar_size;
		flags |= BUNDLE_FLAG_ADMINISTRATIVE_RECORD;
	}

	const uint64_t time_ms = hal_time_get_timestamp_ms();
	const uint64_t seqnum = allocate_sequence_number(
		config,
		time_ms
	);

	struct bundle *bundle;

	if (config->parent->bp_version == 6)
		bundle = bundle6_create_local(
			payload_data,
			payload_length,
			config->registered_eid,
			msg->dst_eid,
			time_ms, seqnum,
			config->parent->lifetime_ms,
			flags
		);
	else
		bundle = bundle7_create_local(
			payload_data,
			payload_length,
			config->registered_eid,
			msg->dst_eid,
			time_ms,
			seqnum,
			config->parent->lifetime_ms,
			flags
		);

	if (!bundle) {
		LOG_WARN("AAP2Agent: Bundle creation failed!");
		return aap2_ResponseStatus_RESPONSE_STATUS_ERROR;
	}

	if (adu_flags.with_bdm_auth) {
		if (!config->dispatch_auth) {
			LOG_WARN("AAP2Agent: BDM auth. requested for ADU but not authorized!");
			bundle_free(bundle);
			return aap2_ResponseStatus_RESPONSE_STATUS_INVALID_REQUEST;
		}

		bundle->bdm_auth_validated = true;
	}

	bundle_processor_inform(
		config->parent->bundle_agent_interface->bundle_signaling_queue,
		(struct bundle_processor_signal){
			.type = BP_SIGNAL_BUNDLE_LOCAL_DISPATCH,
			.bundle = bundle,
		}
	);

	LOGF_DEBUG("AAP2Agent: Injected new bundle %p.", bundle);

	response->bundle_headers.src_eid = strdup(config->registered_eid);
	response->bundle_headers.dst_eid = msg->dst_eid;
	msg->dst_eid = NULL;
	response->bundle_headers.payload_length = payload_length;
	response->bundle_headers.creation_timestamp_ms = time_ms;
	response->bundle_headers.sequence_number = seqnum;
	response->bundle_headers.lifetime_ms = config->parent->lifetime_ms;
	response->has_bundle_headers = true;

	return aap2_ResponseStatus_RESPONSE_STATUS_SUCCESS;
}

static aap2_ResponseStatus process_link_msg(
	struct aap2_agent_comm_config *const config,
	aap2_Link * const msg)
{
	LOGF_DEBUG(
		"AAP2Agent: Received FIB update request wih LinkStatus %d for peer: node_id = \"%s\", cla_addr = \"%s\".",
		msg->status,
		msg->peer_node_id,
		msg->peer_cla_addr
	);

	struct fib_request *req = malloc(sizeof(struct fib_request));

	if (!req)
		return aap2_ResponseStatus_RESPONSE_STATUS_ERROR;

	// Intended status. UNSPECIFIED means to only create the mapping,
	// and not create or change any existing CLA state.
	switch (msg->status) {
	case aap2_LinkStatus_LINK_STATUS_UP:
		req->type = FIB_REQUEST_CREATE_LINK;
		break;
	case aap2_LinkStatus_LINK_STATUS_DOWN:
		req->type = FIB_REQUEST_DROP_LINK;
		break;
	case aap2_LinkStatus_LINK_STATUS_DROP_CLA_MAPPING:
		req->type = FIB_REQUEST_DEL_NODE_ASSOC;
		break;
	default:
		free(req);
		return aap2_ResponseStatus_RESPONSE_STATUS_INVALID_REQUEST;
	}

	if (msg->flag == aap2_LinkFlags_LINK_FLAG_DIRECT)
		req->flags = FIB_ENTRY_FLAG_DIRECT;
	else
		req->flags = FIB_ENTRY_FLAG_NONE;

	req->cla_addr = msg->peer_cla_addr;
	// take ownership
	msg->peer_cla_addr = NULL;

	req->node_id = msg->peer_node_id;
	// take ownership
	msg->peer_node_id = NULL;

	bundle_processor_inform(
		config->parent->bundle_agent_interface->bundle_signaling_queue,
		(struct bundle_processor_signal){
			.type = BP_SIGNAL_FIB_UPDATE_REQUEST,
			.fib_request = req,
		}
	);

	return aap2_ResponseStatus_RESPONSE_STATUS_SUCCESS;
}

static int process_aap_message(
	struct aap2_agent_comm_config *const config,
	aap2_AAPMessage *const msg,
	uint8_t *const payload_data)
{
	aap2_AAPResponse response = aap2_AAPResponse_init_default;
	int result = -1;

	// Default response
	response.response_status = (
		aap2_ResponseStatus_RESPONSE_STATUS_INVALID_REQUEST
	);

	switch (msg->which_msg) {
	case aap2_AAPMessage_config_tag:
		response.response_status = process_configure_msg(
			config,
			&msg->msg.config
		);
		break;
	case aap2_AAPMessage_adu_tag:
		response.response_status = process_adu_msg(
			config,
			&msg->msg.adu,
			payload_data,
			&response
		);
		break;
	case aap2_AAPMessage_keepalive_tag:
		LOGF_DEBUG(
			"AAP2Agent: Received KEEPALIVE from \"%s\"",
			config->registered_eid
			? config->registered_eid
			: "<not registered>"
		);
		response.response_status = (
			aap2_ResponseStatus_RESPONSE_STATUS_ACK
		);
		break;
	case aap2_AAPMessage_link_tag:
		if (!config->fib_auth)
			break;
		response.response_status = process_link_msg(
			config,
			&msg->msg.link
		);
		break;
	default:
		LOGF_WARN(
			"AAP2Agent: Cannot handle AAP messages of tag type %d!",
			msg->which_msg
		);
		break;
	}

	result = send_message(
		config->socket_fd,
		aap2_AAPResponse_fields,
		&response
	);

	// Free message contents
	pb_release(aap2_AAPResponse_fields, &response);
	return result;
}

static uint8_t *receive_payload(pb_istream_t *istream, size_t payload_length)
{
	if (payload_length > BUNDLE_MAX_SIZE) {
		LOG_WARN("AAP2Agent: Payload too large!");
		return NULL;
	}

	uint8_t *payload = malloc(payload_length);

	if (!payload) {
		LOG_ERROR("AAP2Agent: Payload alloc error!");
		return NULL;
	}


	const bool success = pb_read(istream, payload, payload_length);

	if (!success) {
		free(payload);
		payload = NULL;
		LOG_ERROR("AAP2Agent: Payload read error!");
	}

	return payload;
}

static size_t set_adu_flags(
	aap2_BundleADUFlags *flags_field, struct bundle_adu_flags flags)
{
	size_t count = 0;

	if (flags.bpdu)
		flags_field[count++] = aap2_BundleADUFlags_BUNDLE_ADU_BPDU;
	if (flags.with_bdm_auth)
		flags_field[count++] = (
			aap2_BundleADUFlags_BUNDLE_ADU_WITH_BDM_AUTH
		);

	return count;
}

static int send_bundle_from_queue(struct aap2_agent_comm_config *const config,
				  struct bundle_adu data)
{
	struct bundle_adu_flags flags = {
		.bpdu = (data.proc_flags == BUNDLE_FLAG_ADMINISTRATIVE_RECORD),
		.with_bdm_auth = data.bdm_auth_validated,
	};
	aap2_AAPMessage msg = aap2_AAPMessage_init_default;

	msg.which_msg = aap2_AAPMessage_adu_tag;
	msg.msg.adu.dst_eid = data.destination;
	msg.msg.adu.src_eid = data.source;
	msg.msg.adu.payload_length = data.length;
	msg.msg.adu.creation_timestamp_ms = data.bundle_creation_timestamp_ms;
	msg.msg.adu.sequence_number = data.bundle_sequence_number;
	msg.msg.adu.adu_flags_count = set_adu_flags(
		msg.msg.adu.adu_flags, flags
	);

	const int socket_fd = config->socket_fd;
	int send_result = send_message(
		socket_fd,
		aap2_AAPMessage_fields,
		&msg
	);

	// NOTE: We do not "release" msg as there is nothing to deallocate here.

	if (send_result < 0) {
		bundle_adu_free_members(data);
		return send_result;
	}

	struct tcp_write_to_socket_param wsp = {
		.socket_fd = socket_fd,
		.errno_ = 0,
	};
	pb_ostream_t stream = {
		&write_callback,
		&wsp,
		SIZE_MAX,
		0,
		NULL,
	};

	const bool ret = pb_write(&stream, data.payload, data.length);

	if (wsp.errno_) {
		LOG_ERRNO("AAP2Agent", "send()", wsp.errno_);
		send_result = -1;
	} else if (!ret) {
		LOGF_WARN(
			"AAP2Agent: pb_write() error: %s",
			PB_GET_ERROR(&stream)
		);
		send_result = -1;
	}

	if (send_result < 0) {
		bundle_adu_free_members(data);
		return send_result;
	}

	// Receive status from client.
	aap2_AAPResponse response = aap2_AAPResponse_init_default;

	if (poll_recv_timeout(config->socket_fd, AAP2_AGENT_TIMEOUT_MS) <= 0) {
		LOG_WARN("AAP2Agent: No response received, closing connection.");
		send_result = -1;
		goto done;
	}

	const bool success = pb_decode_ex(
		&config->pb_istream,
		aap2_AAPResponse_fields,
		&response,
		PB_DECODE_DELIMITED
	);

	if (!success) {
		LOGF_WARN(
			"AAP2Agent: Protobuf decode error: %s",
			PB_GET_ERROR(&config->pb_istream)
		);
		send_result = -1;
		goto done;
	}

	if (response.response_status !=
	    aap2_ResponseStatus_RESPONSE_STATUS_SUCCESS) {
		// TODO: Implement configurable policy to re-route/keep somehow.
		LOG_WARN("AAP2Agent: Client reported error for bundle, dropping.");
		// NOTE: Do not return -1 here as this would close the socket.
	}

done:
	pb_release(aap2_AAPResponse_fields, &response);
	bundle_adu_free_members(data);
	return send_result;
}

static int send_keepalive(struct aap2_agent_comm_config *const config)
{
	aap2_AAPMessage msg = aap2_AAPMessage_init_default;

	LOG_DEBUG("AAP2Agent: Sending Keepalive message to Client.");
	msg.which_msg = aap2_AAPMessage_keepalive_tag;

	const int socket_fd = config->socket_fd;
	int send_result = send_message(
		socket_fd,
		aap2_AAPMessage_fields,
		&msg
	);

	// NOTE: We do not "release" msg as there is nothing to deallocate here.

	if (send_result < 0)
		return send_result;

	// Receive status from client.
	aap2_AAPResponse response = aap2_AAPResponse_init_default;

	if (poll_recv_timeout(config->socket_fd, AAP2_AGENT_TIMEOUT_MS) <= 0) {
		LOG_WARN("AAP2Agent: No response received, closing connection.");
		send_result = -1;
		goto done;
	}

	const bool success = pb_decode_ex(
		&config->pb_istream,
		aap2_AAPResponse_fields,
		&response,
		PB_DECODE_DELIMITED
	);

	if (!success) {
		LOGF_WARN(
			"AAP2Agent: Protobuf decode error: %s",
			PB_GET_ERROR(&config->pb_istream)
		);
		send_result = -1;
		goto done;
	}

	if (response.response_status !=
	    aap2_ResponseStatus_RESPONSE_STATUS_ACK) {
		LOG_WARN("AAP2Agent: Keepalive not acknowledged, closing connection.");
		send_result = -1;
		goto done;
	}

done:
	pb_release(aap2_AAPResponse_fields, &response);
	return send_result;
}

static int dispatch_from_queue(struct aap2_agent_comm_config *const config,
			       struct dispatch_info data)
{
	struct dispatch_result *dispatch_result;

	aap2_AAPMessage msg = aap2_AAPMessage_init_default;
	aap2_DispatchEvent *req = &msg.msg.dispatch_event;

	msg.which_msg = aap2_AAPMessage_dispatch_event_tag;
	req->bundle.src_eid = data.bundle->source;
	req->bundle.dst_eid = data.bundle->destination;
	req->bundle.creation_timestamp_ms = data.bundle->creation_timestamp_ms;
	req->bundle.sequence_number = data.bundle->sequence_number;
	req->bundle.payload_length = data.bundle->payload_block->length;
	req->bundle.fragment_offset = data.bundle->fragment_offset;
	req->bundle.total_adu_length = data.bundle->total_adu_length;
	req->bundle.lifetime_ms = data.bundle->lifetime_ms;
	req->has_bundle = true;

	aap2_BundleDispatchInfo * const di = &req->additional_information;

	di->serialized_size = bundle_get_serialized_size(
		data.bundle
	);
	if (HAS_FLAG(data.bundle->proc_flags, BUNDLE_FLAG_MUST_NOT_BE_FRAGMENTED)) {
		di->min_frag_size_first = di->serialized_size;
		di->min_frag_size_last = 0;
	} else {
		di->min_frag_size_first = bundle_get_first_fragment_min_size(
			data.bundle
		);
		di->min_frag_size_last = bundle_get_last_fragment_min_size(
			data.bundle
		);
	}
	di->dispatch_time_ms = hal_time_get_timestamp_ms();
	di->expiration_time_ms = bundle_get_expiration_time_ms(
		data.bundle
	);
	di->dispatched_to_node_id = data.orig_node_id;
	di->dispatched_to_cla_addr = data.orig_cla_addr;
	di->max_bundle_size_bytes = config->parent->max_bundle_size_bytes;
	req->has_additional_information = true;

	switch (data.reason) {
	case DISPATCH_REASON_NO_FIB_ENTRY:
		req->reason = aap2_DispatchReason_DISPATCH_REASON_NO_FIB_ENTRY;
		break;
	case DISPATCH_REASON_LINK_INACTIVE:
		req->reason = aap2_DispatchReason_DISPATCH_REASON_LINK_INACTIVE;
		break;
	case DISPATCH_REASON_CLA_ENQUEUE_FAILED:
		req->reason =
			aap2_DispatchReason_DISPATCH_REASON_CLA_ENQUEUE_FAILED;
		break;
	case DISPATCH_REASON_TX_FAILED:
		req->reason = aap2_DispatchReason_DISPATCH_REASON_TX_FAILED;
		break;
	case DISPATCH_REASON_TX_SUCCEEDED:
		req->reason = aap2_DispatchReason_DISPATCH_REASON_TX_SUCCEEDED;
		break;
	default:
		req->reason = aap2_DispatchReason_DISPATCH_REASON_UNSPECIFIED;
		break;
	}

	const int socket_fd = config->socket_fd;
	int send_result = send_message(
		socket_fd,
		aap2_AAPMessage_fields,
		&msg
	);

	free(data.orig_node_id);
	free(data.orig_cla_addr);

	if (send_result < 0)
		goto failure;

	if (poll_recv_timeout(config->socket_fd, AAP2_AGENT_TIMEOUT_MS) <= 0) {
		LOG_WARN("AAP2Agent: No response received from BDM agent, closing connection.");
		send_result = -1;
		goto failure;
	}

	aap2_AAPResponse response;

	const bool success = pb_decode_ex(
		&config->pb_istream,
		aap2_AAPResponse_fields,
		&response,
		PB_DECODE_DELIMITED
	);

	if (!success) {
		LOGF_WARN(
			"AAP2Agent: Protobuf decode error: %s",
			PB_GET_ERROR(&config->pb_istream)
		);
		goto failure;
	}

	if (response.response_status !=
	    aap2_ResponseStatus_RESPONSE_STATUS_SUCCESS) {
		LOG_WARN("AAP2Agent: BDM reported failure.");
		pb_release(aap2_AAPResponse_fields, &response);
		goto failure;
	}

	if (!response.has_dispatch_result ||
	    response.dispatch_result.next_hops_count == 0) {
		LOG_DEBUG("AAP2Agent: BDM provided an empty result (= drop).");
		pb_release(aap2_AAPResponse_fields, &response);
		goto failure;
	}

	dispatch_result = malloc(
		sizeof(struct dispatch_result)
	);

	if (!dispatch_result) {
		LOG_WARN("AAP2Agent: Memory allocation failed.");
		pb_release(aap2_AAPResponse_fields, &response);
		goto failure;
	}

	dispatch_result->next_hops = malloc(
		sizeof(struct dispatch_next_hop[
			response.dispatch_result.next_hops_count])
	);

	if (!dispatch_result->next_hops) {
		LOG_WARN("AAP2Agent: Memory allocation failed.");
		free(dispatch_result);
		pb_release(aap2_AAPResponse_fields, &response);
		goto failure;
	}

	LOGF_DEBUG(
		"AAP2Agent: BDM response for bundle %p indicates %d next hop(s).",
		data.bundle,
		response.dispatch_result.next_hops_count
	);
	dispatch_result->orig_reason = data.reason;
	dispatch_result->next_hop_count =
		response.dispatch_result.next_hops_count;
	for (int i = 0; i < response.dispatch_result.next_hops_count; i++) {
		aap2_DispatchResult_NextHopEntry nhe =
			response.dispatch_result.next_hops[i];
		dispatch_result->next_hops[i] = (struct dispatch_next_hop){
			.node_id = strdup(nhe.node_id),
			.fragment_offset = nhe.fragment_offset,
			.fragment_length = nhe.fragment_length,
		};
	}

	pb_release(aap2_AAPResponse_fields, &response);

	bundle_processor_inform(
		config->parent->bundle_agent_interface->bundle_signaling_queue,
		(struct bundle_processor_signal){
			.type = BP_SIGNAL_BUNDLE_BDM_DISPATCH,
			.bundle = data.bundle,
			.dispatch_result = dispatch_result,
		}
	);

	// cppcheck-suppress memleak
	return 0;

failure:
	dispatch_result = malloc(sizeof(struct dispatch_result));

	if (dispatch_result) {
		dispatch_result->orig_reason = data.reason;
		dispatch_result->next_hop_count = 0;
		dispatch_result->next_hops = NULL;
	}

	bundle_processor_inform(
		config->parent->bundle_agent_interface->bundle_signaling_queue,
		(struct bundle_processor_signal){
			.type = BP_SIGNAL_BUNDLE_BDM_DISPATCH,
			.bundle = data.bundle,
			.dispatch_result = dispatch_result,
		}
	);

	return send_result;
}

static int send_fib_update_from_queue(
	struct aap2_agent_comm_config *const config,
	struct fib_association data)
{
	aap2_AAPMessage msg = aap2_AAPMessage_init_default;

	msg.which_msg = aap2_AAPMessage_link_tag;
	msg.msg.link.peer_cla_addr = data.cla_addr;
	msg.msg.link.peer_node_id = data.node_id;
	switch (data.status) {
	case FIB_LINK_STATUS_UNSPECIFIED:
		msg.msg.link.status = aap2_LinkStatus_LINK_STATUS_UNSPECIFIED;
		break;
	case FIB_LINK_STATUS_UP:
		msg.msg.link.status = aap2_LinkStatus_LINK_STATUS_UP;
		break;
	case FIB_LINK_STATUS_DOWN:
		msg.msg.link.status = aap2_LinkStatus_LINK_STATUS_DOWN;
		break;
	default:
		msg.msg.link.status = aap2_LinkStatus_LINK_STATUS_UNSPECIFIED;
		break;
	}
	if ((data.flags & FIB_ENTRY_FLAG_DIRECT) != 0)
		msg.msg.link.flag = aap2_LinkFlags_LINK_FLAG_DIRECT;

	LOGF_DEBUG(
		"AAP2Agent: Pushing FIB update for \"%s\" via \"%s\", status = %d, flags = 0x%x",
		data.node_id ? data.node_id : "(null)",
		data.cla_addr, data.status, data.flags
	);

	const int socket_fd = config->socket_fd;
	const int send_result = send_message(
		socket_fd,
		aap2_AAPMessage_fields,
		&msg
	);

	free(data.cla_addr);
	free(data.node_id);

	if (send_result < 0)
		return send_result;

	if (poll_recv_timeout(config->socket_fd, AAP2_AGENT_TIMEOUT_MS) <= 0) {
		LOG_WARN("AAP2Agent: No response received from FIB agent, closing connection.");
		return -1;
	}

	aap2_AAPResponse response;

	const bool success = pb_decode_ex(
		&config->pb_istream,
		aap2_AAPResponse_fields,
		&response,
		PB_DECODE_DELIMITED
	);

	if (!success) {
		LOGF_WARN(
			"AAP2Agent: Protobuf decode error: %s",
			PB_GET_ERROR(&config->pb_istream)
		);
		return -1;
	}

	if (response.response_status !=
	    aap2_ResponseStatus_RESPONSE_STATUS_SUCCESS) {
		LOG_WARN("AAP2Agent: FIB update client reported failure.");
		pb_release(aap2_AAPResponse_fields, &response);
		//return send_result;
		// NOTE: Succeed in this case.
	}

	pb_release(aap2_AAPResponse_fields, &response);

	return send_result;
}

static void shutdown_queue(const struct bundle_agent_interface *bai,
			   QueueIdentifier_t agent_queue)
{
	struct aap2_agent_command cmd;
	struct dispatch_result *dispatch_result;

	// NOTE: It is already ensured that nothing will continue writing into
	// the queue when this function is called, as the sink was de-registered
	// and the BP has confirmed it already.
	while (hal_queue_receive(agent_queue, &cmd, 0) == UD3TN_OK) {
		switch (cmd.type) {
		default:
			break;
		case AAP2_ACMD_ADU:
			LOGF_WARN(
				"AAP2Agent: Dropping unsent bundle from '%s'.",
				cmd.bundle_adu.source
			);
			bundle_adu_free_members(cmd.bundle_adu);
			break;
		case AAP2_ACMD_FIB:
			LOGF_WARN(
				"AAP2Agent: Dropping FIB update for '%s', agent disconnected.",
				cmd.fib_assoc.node_id
			);
			free(cmd.fib_assoc.node_id);
			free(cmd.fib_assoc.cla_addr);
			break;
		case AAP2_ACMD_DISPATCH:
			LOGF_WARN(
			"AAP2Agent: Rejecting dispatch of bundle %p, agent disconnected.",
				cmd.disp_info.bundle
			);
			dispatch_result = malloc(
				sizeof(struct dispatch_result)
			);
			if (!dispatch_result)
				break;
			dispatch_result->orig_reason = cmd.disp_info.reason;
			dispatch_result->next_hop_count = 0;
			bundle_processor_inform(
				bai->bundle_signaling_queue,
				(struct bundle_processor_signal){
					.type = BP_SIGNAL_BUNDLE_BDM_DISPATCH,
					.bundle = cmd.disp_info.bundle,
					.dispatch_result = dispatch_result,
				}
			);
			break;
		}
	}
}

static void aap2_agent_comm_task(void *const param)
{
	struct aap2_agent_comm_config *const config = param;

	// Send indicator for invalid version so any AAPv1 client notices.
	const uint8_t aap2_version_indicator = 0x2F;

	if (tcp_send_all(config->socket_fd, &aap2_version_indicator, 1) != 1) {
		LOG_ERRNO("AAP2Agent", "send()", errno);
		goto done;
	}

	char *local_eid = config->parent->bundle_agent_interface->local_eid;
	aap2_AAPMessage msg = aap2_AAPMessage_init_default;

	msg.which_msg = aap2_AAPMessage_welcome_tag;
	msg.msg.welcome.node_id = local_eid;

	if (send_message(config->socket_fd, aap2_AAPMessage_fields, &msg))
		goto done;
	// NOTE: We do not "release" msg as there is nothing to deallocate here.

	aap2_AAPMessage request;
	struct aap2_agent_command cmd;

	for (;;) {
		if (config->is_subscriber) {
			const enum ud3tn_result rv = hal_queue_receive(
				config->agent_queue,
				&cmd,
				config->keepalive_timeout_ms
			);

			if (rv != UD3TN_OK) {
				if (send_keepalive(config) < 0)
					break;
				continue;
			}

			if (cmd.type == AAP2_ACMD_ADU) {
				if (send_bundle_from_queue(config,
							   cmd.bundle_adu) < 0)
					break;
			} else if (cmd.type == AAP2_ACMD_DISPATCH) {
				if (dispatch_from_queue(config,
							cmd.disp_info) < 0)
					break;
			} else if (cmd.type == AAP2_ACMD_FIB) {
				if (send_fib_update_from_queue(config,
							       cmd.fib_assoc) < 0)
					break;
			} else {
				if (cmd.type != AAP2_ACMD_FINALIZE)
					LOG_WARN("AAP2Agent: Invalid command received, terminating.");
				break;
			}
		} else {
			bool success = pb_decode_ex(
				&config->pb_istream,
				aap2_AAPMessage_fields,
				&request,
				PB_DECODE_DELIMITED
			);

			if (!success) {
				if (config->pb_istream.bytes_left == 0)
					LOG_INFO("AAP2Agent: Connection closed by client.");
				else
					LOGF_WARN(
						"AAP2Agent: Protobuf decode error: %s",
						PB_GET_ERROR(
							&config->pb_istream
						)
					);
				break;
			}

			uint8_t *payload = NULL;

			// Read payload for ADU messages. Note that even if this
			// fails we process the message with payload == NULL, to
			// send a proper response.
			if (request.which_msg == aap2_AAPMessage_adu_tag) {
				payload = receive_payload(
					&config->pb_istream,
					request.msg.adu.payload_length
				);
			}

			success = !process_aap_message(
				config,
				&request,
				payload
			);

			pb_release(aap2_AAPMessage_fields, &request);
			if (!success)
				break;
		}
	}

done:
	LOG_DEBUG("AAP2Agent: Comm. task terminating gracefully.");
	// Cleanly shut down the socket.
	shutdown(config->socket_fd, SHUT_RDWR);
#ifdef __APPLE__
	close(config->socket_fd);
#endif // __APPLE__
	// NOTE: This ensures the BP does not send bundles, FIB updates, or
	// dispatch requests to us after the function has returned.
	deregister_sink(config);
	shutdown_queue(
		config->parent->bundle_agent_interface,
		config->agent_queue
	);
	// Clean up tracking information for this task.
	struct aap2_agent_config *const aacfg = config->parent;

	hal_semaphore_take_blocking(aacfg->termination_flag_mutex);
	hal_semaphore_release(config->agent_conn_mutex);
	if (!aacfg->termination_flag) {
		hal_semaphore_take_blocking(aacfg->conn_list_mutex);
		struct aap2_agent_connection_list **cur = (
			&aacfg->conn_list
		);

		// Search for our entry and clean it up.
		while (*cur) {
			if ((*cur)->config == config) {
				struct aap2_agent_connection_list *e = *cur;

				(*cur) = (*cur)->next;
				// NOTE: This frees `config`.
				clean_up_comm_task_resources(e);
				break;
			}
			cur = &(*cur)->next;
		}
		hal_semaphore_release(aacfg->conn_list_mutex);
	}
	hal_semaphore_release(aacfg->termination_flag_mutex);
	LOG_DEBUG("AAP2Agent: Comm. task ended.");
}

struct aap2_agent_config *aap2_agent_setup(
	const struct bundle_agent_interface *bundle_agent_interface,
	const char *socket_path,
	const char *node, const char *service,
	const uint8_t bp_version, const uint64_t lifetime_ms,
	const uint64_t max_bundle_size_bytes)
{
	struct aap2_agent_config *const config = malloc(
		sizeof(struct aap2_agent_config)
	);

	if (!config) {
		LOG_ERROR("AAP2Agent: Error allocating memory for task config!");
		return NULL;
	}

	if (node && service) {
		config->listen_socket =
			create_tcp_socket(node, service, false, NULL);
	} else if (socket_path) {
		config->listen_socket =
			create_unix_domain_socket(socket_path);
	} else {
		LOG_ERROR("AAP2Agent: Invalid socket provided!");
		goto fail_socket;
	}

	if (config->listen_socket < 0)  {
		LOG_ERROR("AAP2Agent: Error binding to provided address!");
		goto fail_socket;
	}

	if (listen(config->listen_socket, AAP2_AGENT_LISTEN_BACKLOG) < 0) {
		LOG_ERRNO_ERROR(
			"AAP2Agent",
			"Error listening on provided address!",
			errno
		);
		goto fail_socket;
	}

	if (node && service)
		LOGF_INFO("AAP2Agent: Listening on [%s]:%s", node, service);
	else
		LOGF_INFO("AAP2Agent: Listening on %s", socket_path);

	config->bundle_agent_interface = bundle_agent_interface;
	config->bp_version = bp_version;
	config->lifetime_ms = lifetime_ms;
	config->max_bundle_size_bytes = max_bundle_size_bytes;

	config->conn_list = NULL;
	config->conn_list_mutex = hal_semaphore_init_binary();

	if (!config->conn_list_mutex) {
		LOG_ERROR("AAP2Agent: Error creating list mutex!");
		goto fail_mutex_1;
	}

	config->termination_flag = false;
	config->termination_flag_mutex = hal_semaphore_init_binary();

	if (!config->termination_flag_mutex) {
		LOG_ERROR("AAP2Agent: Error creating list mutex!");
		goto fail_mutex_2;
	}

	const enum ud3tn_result task_creation_result = hal_task_create(
		aap2_agent_listener_task,
		config,
		false,
		&config->listener_task
	);

	if (task_creation_result != UD3TN_OK) {
		LOG_ERROR("AAP2Agent: Error creating listener task!");
		goto fail_task;
	}

	hal_semaphore_release(config->conn_list_mutex);
	hal_semaphore_release(config->termination_flag_mutex);

	return config;

fail_task:
	hal_semaphore_delete(config->termination_flag_mutex);
fail_mutex_2:
	hal_semaphore_delete(config->conn_list_mutex);
fail_mutex_1:
fail_socket:
	free(config);
	return NULL;
}

void aap2_agent_terminate(struct aap2_agent_config *const config)
{
	shutdown(config->listen_socket, SHUT_RDWR);
#ifdef __APPLE__
	// We have to force it on MacOS.
	close(config->listen_socket);
#endif // __APPLE__
	hal_task_wait(config->listener_task);
	hal_semaphore_delete(config->conn_list_mutex);
	hal_semaphore_delete(config->termination_flag_mutex);
	free(config);
	LOG_DEBUG("AAP2Agent: Terminated gracefully");
}
