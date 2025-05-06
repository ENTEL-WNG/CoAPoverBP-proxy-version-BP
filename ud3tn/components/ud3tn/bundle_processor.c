// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "ud3tn/agent_manager.h"
#include "ud3tn/bundle_fragmenter.h"
#include "ud3tn/bundle_processor.h"
#include "ud3tn/common.h"
#include "ud3tn/eid.h"
#include "ud3tn/fib.h"
#include "ud3tn/report_manager.h"
#include "ud3tn/result.h"
#include "ud3tn/simplehtab.h"

#include "bundle6/bundle6.h"
#include "bundle7/bundle_age.h"
#include "bundle7/hopcount.h"

#include "cla/cla.h"
#include "cla/cla_contact_tx_task.h"

#include "platform/hal_io.h"
#include "platform/hal_queue.h"
#include "platform/hal_semaphore.h"
#include "platform/hal_task.h"
#include "platform/hal_time.h"

#include "cbor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum bundle_handling_result {
	BUNDLE_HRESULT_OK = 0,
	BUNDLE_HRESULT_DELETED,
	BUNDLE_HRESULT_BLOCK_DISCARDED,
};

struct bp_context {
	struct agent_manager *agent_manager;
	const char *local_eid;
	char *local_eid_prefix;
	uint64_t maximum_bundle_size_bytes;
	bool local_eid_is_ipn;
	bool status_reporting;

	struct fib *fib;

	struct reassembly_list {
		struct reassembly_bundle_list {
			struct bundle *bundle;
			struct reassembly_bundle_list *next;
		} *bundle_list;
		struct reassembly_list *next;
	} *reassembly_list;

	struct known_bundle_list {
		struct bundle_unique_identifier id;
		uint64_t deadline_ms;
		struct known_bundle_list *next;
	} *known_bundle_list;
};

/* DECLARATIONS */

static inline void handle_signal(
	struct bp_context *const ctx,
	const struct bundle_processor_signal signal);

static void inform_link_status_change(
	struct bp_context *const ctx,
	const char *cla_addr,
	enum fib_link_status);
static bool send_fib_to_agent(
	void *context, const char *node_id,
	const struct fib_entry *entry,
	const struct fib_link *link);

static enum ud3tn_result bundle_dispatch(
	struct bp_context *const ctx, struct bundle *bundle);
static bool bundle_endpoint_is_local(
	const struct bp_context *const ctx, const struct bundle *bundle);
static enum ud3tn_result bundle_forward(
	const struct bp_context *const ctx, struct bundle *bundle);
static void bundle_forwarding_success(
	const struct bp_context *const ctx, struct bundle *bundle);
static void bundle_forwarding_contraindicated(
	const struct bp_context *const ctx,
	struct bundle *bundle, enum bundle_status_report_reason reason);
static void bundle_forwarding_failed(
	const struct bp_context *const ctx,
	struct bundle *bundle, enum bundle_status_report_reason reason);
static void bundle_expired(
	const struct bp_context *const ctx, struct bundle *bundle);
static void bundle_receive(
	struct bp_context *const ctx, struct bundle *bundle);
static enum bundle_handling_result handle_unknown_block_flags(
	const struct bp_context *const ctx,
	const struct bundle *bundle, enum bundle_block_flags flags);
static void bundle_deliver_local(
	struct bp_context *const ctx, struct bundle *bundle);
static void bundle_attempt_reassembly(
	struct bp_context *const ctx, struct bundle *bundle);
static void bundle_deliver_adu(
	const struct bp_context *const ctx, struct bundle_adu data);
static void bundle_delete(
	const struct bp_context *const ctx,
	struct bundle *bundle, enum bundle_status_report_reason reason);
static void bundle_discard(struct bundle *bundle);
static void bundle_handle_custody_signal(
	struct bundle_administrative_record *signal);
static bool hop_count_validation(struct bundle *bundle);
static const char *get_agent_id(
	const struct bp_context *const ctx, const char *dest_eid);
static bool bundle_record_add_and_check_known(
	struct bp_context *const ctx, const struct bundle *bundle);
static bool bundle_reassembled_is_known(
	struct bp_context *const ctx, const struct bundle *bundle);
static void bundle_add_reassembled_as_known(
	struct bp_context *const ctx, const struct bundle *bundle);

static void send_status_report(
	const struct bp_context *const ctx,
	const struct bundle *bundle,
	const enum bundle_status_report_status_flags status,
	const enum bundle_status_report_reason reason);
static enum ud3tn_result send_bundle(
	const struct bp_context *const ctx, struct bundle *bundle,
	const char *next_hop);

static inline void bundle_add_rc(struct bundle *bundle,
	const enum bundle_retention_constraints constraint)
{
	bundle->ret_constraints |= constraint;
}

static inline void bundle_rem_rc(struct bundle *bundle,
	const enum bundle_retention_constraints constraint, int discard)
{
	bundle->ret_constraints &= ~constraint;
	if (discard && bundle->ret_constraints == BUNDLE_RET_CONSTRAINT_NONE)
		bundle_discard(bundle);
}

/* COMMUNICATION */

void bundle_processor_inform(
	QueueIdentifier_t bundle_processor_signaling_queue,
	const struct bundle_processor_signal signal)
{
	// TODO: If moving the AAP agent to simple_queue, ensure keeping at
	// least one slot free for the dispatcher here
	// (only BDMs may fill the queue completely) to prevent blocking!
	hal_queue_push_to_back(bundle_processor_signaling_queue, &signal);
}

agent_no_t bundle_processor_perform_agent_action(
	QueueIdentifier_t signaling_queue,
	enum bundle_processor_signal_type type,
	const struct agent agent)
{
	ASSERT(type == BP_SIGNAL_AGENT_REGISTER ||
	       type == BP_SIGNAL_AGENT_DEREGISTER);

	struct agent_manager_parameters *const aaps = malloc(
		sizeof(struct agent_manager_parameters)
	);

	if (!aaps)
		return AGENT_NO_INVALID;

	aaps->agent = agent;
	aaps->feedback_queue = NULL;

	struct bundle_processor_signal signal = {
		.type = type,
		.agent_manager_params = aaps,
	};

	agent_no_t result;
	QueueIdentifier_t feedback_queue = hal_queue_create(
		1,
		sizeof(agent_no_t)
	);

	if (!feedback_queue) {
		free(aaps);
		return AGENT_NO_INVALID;
	}

	aaps->feedback_queue = feedback_queue;
	bundle_processor_inform(signaling_queue, signal);

	if (hal_queue_receive(feedback_queue, &result, -1) == UD3TN_OK) {
		hal_queue_delete(feedback_queue);
		return result;
	}

	hal_queue_delete(feedback_queue);
	// hal_queue_receive with timeout == -1 should never fail, continue
	// anyway in release mode
	ASSERT(false);
	return AGENT_NO_INVALID;
}

int bundle_processor_perform_agent_action_async(
	QueueIdentifier_t signaling_queue,
	enum bundle_processor_signal_type type,
	const struct agent agent)
{
	ASSERT(type == BP_SIGNAL_AGENT_REGISTER ||
	       type == BP_SIGNAL_AGENT_DEREGISTER);

	struct agent_manager_parameters *const aaps = malloc(
		sizeof(struct agent_manager_parameters)
	);

	if (!aaps)
		return -1;

	aaps->agent = agent;
	aaps->feedback_queue = NULL;

	struct bundle_processor_signal signal = {
		.type = type,
		.agent_manager_params = aaps,
	};

	bundle_processor_inform(signaling_queue, signal);

	return 0;
}

void bundle_processor_task(void * const param)
{
	struct bundle_processor_task_parameters *p =
		(struct bundle_processor_task_parameters *)param;
	struct bundle_processor_signal signal;
	struct bp_context ctx = {
		.agent_manager = p->agent_manager,
		.local_eid = p->local_eid,
		.local_eid_prefix = NULL,
		.maximum_bundle_size_bytes = p->maximum_bundle_size_bytes,
		.status_reporting = p->status_reporting,
		.reassembly_list = NULL,
		.known_bundle_list = NULL,
	};

	ctx.fib = fib_alloc();
	ASSERT(ctx.fib != NULL);

	ASSERT(strlen(ctx.local_eid) > 3);

	const enum eid_scheme local_eid_scheme = get_eid_scheme(ctx.local_eid);

	if (local_eid_scheme == EID_SCHEME_IPN) {
		ctx.local_eid_is_ipn = true;
		ctx.local_eid_prefix = strdup(ctx.local_eid);
		ASSERT(ctx.local_eid_prefix != NULL);
		if (ctx.local_eid_prefix == NULL)
			abort();

		char *const dot = strchr(ctx.local_eid_prefix, '.');

		if (!dot) {
			LOGF_ERROR(
				"BundleProcessor: Invalid local EID \"%s\"",
				ctx.local_eid_prefix
			);
			abort(); // should be a bug, is checked beforehand
		} else {
			dot[1] = '\0'; // truncate string after dot
		}
	} else if (local_eid_scheme == EID_SCHEME_DTN) {
		ctx.local_eid_prefix = strdup(ctx.local_eid);
		ASSERT(ctx.local_eid_prefix != NULL);
		if (ctx.local_eid_prefix == NULL)
			abort();

		const size_t len = strlen(ctx.local_eid_prefix);

		// remove slash if it is there to also match EIDs without
		if (ctx.local_eid_prefix[len - 1] == '/')
			ctx.local_eid_prefix[len - 1] = '\0';
	} else {
		// bug
		LOG_ERROR("BundleProcessor: Unknown/invalid local EID scheme!");
		abort();
	}

	LOGF_INFO(
		"BundleProcessor: BPA initialized for \"%s\", status reports %s",
		p->local_eid,
		p->status_reporting ? "enabled" : "disabled"
	);

	for (;;) {
		if (hal_queue_receive(p->signaling_queue, &signal,
			-1) == UD3TN_OK
		) {
			handle_signal(&ctx, signal);
		}
	}
}

static inline void handle_signal(
	struct bp_context *const ctx,
	const struct bundle_processor_signal signal)
{
	struct agent_manager_parameters *aaps;
	agent_no_t agent_feedback;
	struct bundle *bundle;

	switch (signal.type) {
	case BP_SIGNAL_BUNDLE_INCOMING:
		bundle_receive(ctx, signal.bundle);
		break;
	case BP_SIGNAL_TRANSMISSION_SUCCESS:
		if (signal.bundle->parent) {
			ASSERT(!signal.bundle->parent->parent);
			bundle = signal.bundle->parent;
			// We do not process the fragment any further. It was
			// specifically created for the single transmission.
			bundle_free(signal.bundle);
		} else {
			bundle = signal.bundle;
		}
		bundle->forwarding_refcount--;
		LOGF_DEBUG(
			"BundleProcessor: TX of bundle %p to %s via %s OK (-> %d references)",
			bundle,
			signal.peer_node_id,
			signal.peer_cla_addr,
			bundle->forwarding_refcount
		);
		// If the handover fails, declare success immediately.
		if (agent_handover_to_dispatch(
			ctx->agent_manager,
			bundle,
			DISPATCH_REASON_TX_SUCCEEDED,
			signal.peer_node_id,
			signal.peer_cla_addr,
			ctx
		) != 0)
			bundle_forwarding_success(ctx, bundle);
		free(signal.peer_node_id);
		free(signal.peer_cla_addr);
		break;
	case BP_SIGNAL_TRANSMISSION_FAILURE:
		if (signal.bundle->parent) {
			ASSERT(!signal.bundle->parent->parent);
			bundle = signal.bundle->parent;
			// We do not process the fragment any further. It was
			// specifically created for the single transmission.
			bundle_free(signal.bundle);
		} else {
			bundle = signal.bundle;
		}
		bundle->forwarding_refcount--;
		LOGF_DEBUG(
			"BundleProcessor: TX of bundle %p to %s via %s FAILED (-> %d references)",
			bundle,
			signal.peer_node_id,
			signal.peer_cla_addr,
			bundle->forwarding_refcount
		);
		if (agent_handover_to_dispatch(
			ctx->agent_manager,
			bundle,
			DISPATCH_REASON_TX_FAILED,
			signal.peer_node_id,
			signal.peer_cla_addr,
			ctx
		) != 0)
			bundle_forwarding_failed(
				ctx,
				bundle,
				BUNDLE_SR_REASON_TRANSMISSION_CANCELED
			);
		free(signal.peer_node_id);
		free(signal.peer_cla_addr);
		break;
	case BP_SIGNAL_BUNDLE_LOCAL_DISPATCH:
		bundle = signal.bundle;
		bundle_dispatch(ctx, bundle);
		break;
	case BP_SIGNAL_AGENT_REGISTER:
		aaps = signal.agent_manager_params;
		agent_feedback = agent_register(
			ctx->agent_manager,
			aaps->agent
		);
		if (aaps->feedback_queue)
			hal_queue_push_to_back(
				aaps->feedback_queue,
				&agent_feedback
			);

		if (aaps->agent.auth_fib)
			fib_foreach(ctx->fib, send_fib_to_agent, ctx, NULL);
		free(aaps);
		break;
	case BP_SIGNAL_AGENT_DEREGISTER:
		aaps = signal.agent_manager_params;
		agent_feedback = agent_deregister(
			ctx->agent_manager,
			aaps->agent.agent_no
		);
		if (aaps->feedback_queue)
			hal_queue_push_to_back(
				aaps->feedback_queue,
				&agent_feedback
			);
		free(aaps);
		break;
	case BP_SIGNAL_NEW_LINK_ESTABLISHED:
		LOGF_DEBUG(
			"BundleProcessor: Marking link to CLA address \"%s\" as active",
			signal.peer_cla_addr
		);
		inform_link_status_change(
			ctx,
			signal.peer_cla_addr,
			FIB_LINK_STATUS_UP
		);
		free(signal.peer_cla_addr);
		break;
	case BP_SIGNAL_LINK_DOWN:
		LOGF_DEBUG(
			"BundleProcessor: Marking link to CLA address \"%s\" as inactive",
			signal.peer_cla_addr
		);
		inform_link_status_change(
			ctx,
			signal.peer_cla_addr,
			FIB_LINK_STATUS_DOWN
		);
		free(signal.peer_cla_addr);
		break;
	case BP_SIGNAL_BUNDLE_BDM_DISPATCH:
		bundle = signal.bundle;
		bundle_processor_execute_bdm_dispatch(
			ctx,
			bundle,
			signal.dispatch_result
		);
		for (int i = 0; i < signal.dispatch_result->next_hop_count; i++)
			free(signal.dispatch_result->next_hops[i].node_id);
		free(signal.dispatch_result->next_hops);
		free(signal.dispatch_result);
		break;
	case BP_SIGNAL_FIB_UPDATE_REQUEST:
		bundle_processor_handle_fib_update_request(
			ctx,
			signal.fib_request
		);
		free(signal.fib_request->node_id);
		free(signal.fib_request->cla_addr);
		free(signal.fib_request);
		break;
	default:
		LOGF_WARN(
			"BundleProcessor: Invalid signal (%d) detected",
			signal.type
		);
		break;
	}
}

static void inform_link_status_change(
	struct bp_context *const ctx,
	const char *cla_addr,
	enum fib_link_status status)
{
	struct fib_link *const fib_link = fib_insert_link(
		ctx->fib,
		cla_addr
	);

	fib_link->status = status;
	fib_foreach(
		ctx->fib,
		send_fib_to_agent,
		ctx,
		cla_addr
	);
	if (status == FIB_LINK_STATUS_DOWN)
		// This MAY drop the Link, but only if the refcnt is zero.
		fib_remove_link(ctx->fib, cla_addr);
}

void bundle_processor_execute_bdm_dispatch(
	const struct bp_context *const ctx, struct bundle *const bundle,
	const struct dispatch_result *const dispatch_result)
{
	bool send_error = false;

	// Dispatch failed or intentionally returned no next hop?
	if (dispatch_result->next_hop_count == 0) {
		switch (dispatch_result->orig_reason) {
		default:
			bundle_forwarding_contraindicated(
				ctx,
				bundle,
				BUNDLE_SR_REASON_NO_KNOWN_ROUTE
			);
			break;
		case DISPATCH_REASON_TX_FAILED:
			bundle_forwarding_failed(
				ctx,
				bundle,
				BUNDLE_SR_REASON_TRANSMISSION_CANCELED
			);
			break;
		case DISPATCH_REASON_TX_SUCCEEDED:
			bundle_forwarding_success(ctx, bundle);
			break;
		}
		return;
	}

	if (dispatch_result->next_hop_count > BUNDLE_MAX_FRAGMENT_COUNT) {
		LOGF_INFO(
			"BundleProcessor: Number of next hops provided by BDM for bundle %p exceeds maximum fragment count (%u > %u)",
			bundle,
			dispatch_result->next_hop_count,
			BUNDLE_MAX_FRAGMENT_COUNT
		);
		bundle_forwarding_contraindicated(
			ctx,
			bundle,
			BUNDLE_SR_REASON_NO_KNOWN_ROUTE
		);
		return;
	}

	for (int i = 0; i < dispatch_result->next_hop_count; i++) {
		struct dispatch_next_hop nh = dispatch_result->next_hops[i];
		const bool is_to_be_fragmented = (
			(nh.fragment_length != 0 &&
			 nh.fragment_length < bundle->payload_block->length)
		);

		if (is_to_be_fragmented) {
			const uint64_t frag_offset_rel = (
				bundle_is_fragmented(bundle)
				? (nh.fragment_offset < bundle->fragment_offset
				   ? bundle->fragment_offset
				   : (nh.fragment_offset -
				      bundle->fragment_offset))
				: nh.fragment_offset
			);
			struct bundle *f = bundlefragmenter_fragment_bundle(
				bundle,
				frag_offset_rel,
				nh.fragment_length
			);

			if (f == NULL) {
				LOGF_DEBUG(
					"BundleProcessor: Fragmentation failed for bundle %p to %s",
					bundle,
					nh.node_id
				);
				send_error = true;
				continue;
			}

			LOGF_DEBUG(
				"BundleProcessor: Created fragment %p for bundle %p [%llu, %llu], forwarding to %s",
				f,
				bundle,
				frag_offset_rel,
				frag_offset_rel + nh.fragment_length,
				nh.node_id
			);

			f->parent = bundle;
			if (send_bundle(ctx, f, nh.node_id) != UD3TN_OK) {
				send_error = true;
				bundle_free(f);
				continue;
			}
			// Also increment the refcount for the original bundle,
			// which we handle when receiving TX events.
			bundle->forwarding_refcount++;
		} else {
			// NOTE: This function will not do any failure
			// reporting, as a bundle may still be contained in
			// the storage.
			if (send_bundle(ctx, bundle, nh.node_id) != UD3TN_OK)
				send_error = true;
		}
	}

	if (send_error) {
		// NOTE: IMPORTANT: This must only be executed once, as
		// bundle_processor_inform will only keep free one slot and
		// otherwise the looping notification will block.
		// NOTE: Even though `send_bundle` might fail because the
		// specified FIB entry is not present or the link is not active,
		// we report with the "CLA enqueue failed" reason here, because
		// this is what the BDM requested and the rest should be
		// ensured beforehand by it. Otherwise we cannot reliably
		// differentiate in the BDM between an initial request and a
		// failed dispatch due to the FIB entry being removed and the
		// BDM might react as if it performs initial bundle processing.
		if (agent_handover_to_dispatch(
			ctx->agent_manager,
			bundle,
			DISPATCH_REASON_CLA_ENQUEUE_FAILED,
			NULL,
			NULL,
			ctx
		) != 0)
			bundle_forwarding_failed(
				ctx,
				bundle,
				BUNDLE_SR_REASON_TRANSMISSION_CANCELED
			);
	}
}

void bundle_processor_handle_fib_update_request(
	struct bp_context *const ctx,
	struct fib_request *fib_request)
{
	struct fib_entry *entry;
	struct cla_config *cla_config;

	switch (fib_request->type) {
	default:
		LOGF_WARN(
			"BundleProcessor: Invalid FIB request type: %d",
			fib_request->type
		);
		return;
	case FIB_REQUEST_CREATE_LINK:
		entry = fib_insert_node(
			ctx->fib,
			fib_request->node_id,
			fib_request->cla_addr
		);
		ASSERT(entry != NULL);
		if (entry == NULL) {
			LOG_WARN("BundleProcessor: Adding FIB entry failed!");
			return;
		}
		entry->flags = fib_request->flags;

		// Link establishment requested
		cla_config = cla_config_get(
			entry->cla_addr
		);

		if (!cla_config) {
			LOGF_WARN(
				"BundleProcessor: Could not obtain CLA for address \"%s\"",
				fib_request->cla_addr
			);
		} else {
			LOGF_DEBUG(
				"BundleProcessor: New FIB entry: \"%s\" -> \"%s\"",
				fib_request->node_id,
				fib_request->cla_addr
			);
			const enum cla_link_update_result lur = (
				cla_config->vtable->cla_start_scheduled_contact(
					cla_config,
					fib_request->node_id,
					fib_request->cla_addr
				)
			);

			// Treat as successful establishment to trigger agents.
			if (lur == CLA_LINK_UPDATE_UNCHANGED ||
			    lur == CLA_LINK_UPDATE_PERFORMED)
				inform_link_status_change(
					ctx,
					fib_request->cla_addr,
					FIB_LINK_STATUS_UP
				);
		}
		break;
	case FIB_REQUEST_DROP_LINK:
		// Link deestablishment requested
		cla_config = cla_config_get(
			fib_request->cla_addr
		);

		if (!cla_config) {
			LOGF_WARN(
				"BundleProcessor: Could not obtain CLA for address \"%s\"",
				fib_request->cla_addr
			);
		} else {
			LOGF_DEBUG(
				"BundleProcessor: Deleting FIB entry: \"%s\" -> \"%s\"",
				fib_request->node_id,
				fib_request->cla_addr
			);
			// This will call fib_remove_link via
			// BP_SIGNAL_LINK_DOWN.
			cla_config->vtable->cla_end_scheduled_contact(
				cla_config,
				fib_request->node_id,
				fib_request->cla_addr
			);
		}

		bool drop_assoc = true;
		// If a "generic" CLA address has been used, i.e., if there
		// is no concrete CLA next hop being addressed (such as if
		// an incoming connection must be used or it is a single-
		// connection CLA, we do not want to drop all node associations
		// as they might be unrelated.
		const char *cla_conn_addr_offset = strchr(
			fib_request->cla_addr,
			':'
		);

		if (!cla_conn_addr_offset || cla_conn_addr_offset[1] == '\0') {
			LOGF_DEBUG(
				"BundleProcessor: Missing CLA target address (single-connection CLA or only incoming connections), not dropping associated node mappings for CLA address: \"%s\"",
				fib_request->cla_addr
			);
			drop_assoc = false;
		}

		// Drop all nodes associated with the CLA address and delete
		// the Link in case the CLA has no active connection.
		fib_remove_node(ctx->fib, fib_request->node_id, drop_assoc);
		break;
	case FIB_REQUEST_DEL_NODE_ASSOC:
		LOGF_DEBUG(
			"BundleProcessor: Deleting FIB node mapping: \"%s\" -> \"%s\"",
			fib_request->node_id,
			fib_request->cla_addr
		);
		fib_remove_node(ctx->fib, fib_request->node_id, false);
		break;
	}
}

static bool send_fib_to_agent(void *context, const char *node_id,
			      const struct fib_entry *entry,
			      const struct fib_link *link)
{
	struct bp_context *const ctx = context;

	agent_send_fib_info(
		ctx->agent_manager,
		entry,
		node_id,
		link->status,
		ctx
	);
	// NOTE: Returning false here means that fib_foreach will stop
	// iterating, which is not what we want in case no agent was registered
	// for one entry (agent_send_fib_info retuns -1 in this case). Thus,
	// we always return true here.
	return true;
}

/* BUNDLE HANDLING */

/* 5.3 */
static enum ud3tn_result bundle_dispatch(
	struct bp_context *const ctx, struct bundle *bundle)
{
	LOGF_DEBUG(
		"BundleProcessor: Dispatching bundle %p (from = %s, to = %s)",
		bundle,
		bundle->source,
		bundle->destination
	);
	/* 5.3-1 */
	if (bundle_endpoint_is_local(ctx, bundle)) {
		bundle_deliver_local(ctx, bundle);
		return UD3TN_OK;
	}
	/* 5.3-2 */
	return bundle_forward(ctx, bundle);
}

enum ud3tn_result bundle_processor_bundle_dispatch(
	void *bp_context, struct bundle *bundle)
{
	return bundle_dispatch(bp_context, bundle);
}

static bool endpoint_is_local(
	const struct bp_context *const ctx, const char *eid)
{
	const size_t local_len = strlen(ctx->local_eid_prefix);
	const size_t dest_len = strlen(eid);

	/* Compare EID _prefix_ with configured uD3TN EID */
	return (
		// For the memcmp to be safe, the tested EID has to be at
		// least as long as the local EID.
		dest_len >= local_len &&
		// The prefix (the local EID) has to match the EID.
		memcmp(ctx->local_eid_prefix, eid,
		       local_len) == 0
	);
}

/* 5.3-1 */
static bool bundle_endpoint_is_local(
	const struct bp_context *const ctx, const struct bundle *bundle)
{
	return endpoint_is_local(ctx, bundle->destination);
}

/* 5.4 */
static enum ud3tn_result bundle_forward(
	const struct bp_context *const ctx, struct bundle *bundle)
{
	/* 4.3.4. Hop Count (BPv7-bis) */
	if (!hop_count_validation(bundle)) {
		LOGF_DEBUG(
			"BundleProcessor: Deleting bundle %p: Hop Limit Exceeded",
			bundle
		);
		bundle_delete(ctx, bundle, BUNDLE_SR_REASON_HOP_LIMIT_EXCEEDED);
		return UD3TN_FAIL;
	}

	/* 5.4-1 */
	bundle_add_rc(bundle, BUNDLE_RET_CONSTRAINT_FORWARD_PENDING);
	bundle_rem_rc(bundle, BUNDLE_RET_CONSTRAINT_DISPATCH_PENDING, 0);

	/* 5.4-2 */
	// First, try to forward directly
	char *dst_node_id = get_node_id(bundle->destination);
	struct fib_entry *dst_entry = fib_lookup_node(ctx->fib, dst_node_id);
	int rv = -1;
	enum bundle_dispatch_reason dr = DISPATCH_REASON_NO_FIB_ENTRY;

	if (dst_entry && HAS_FLAG(dst_entry->flags, FIB_ENTRY_FLAG_DIRECT)) {
		LOGF_DEBUG(
			"BundleProcessor: Attempting direct dispatch for bundle %p via link to \"%s\".",
			bundle,
			dst_entry->cla_addr
		);

		const struct fib_link *link = fib_lookup_cla_addr(
			ctx->fib,
			dst_entry->cla_addr
		);

		if (!link)
			LOGF_WARN(
				"BundleProcessor: Link to \"%s\" not found in FIB.",
				dst_entry->cla_addr
			);

		if (link && link->status == FIB_LINK_STATUS_UP) {
			if (send_bundle(ctx, bundle, dst_node_id) == UD3TN_OK) {
				// ok, bundle was sent
				rv = 0;
			} else {
				dr = DISPATCH_REASON_CLA_ENQUEUE_FAILED;
			}
		} else {
			dr = DISPATCH_REASON_LINK_INACTIVE;
		}
	}

	if (rv != 0)
		rv = agent_handover_to_dispatch(
			ctx->agent_manager,
			bundle,
			dr,
			NULL,
			NULL,
			ctx
		);

	free(dst_node_id);

	// Could not dispatch externally
	if (rv < 0) {
		LOGF_DEBUG("BundleProcessor: BDM dispatch for bundle %p to \"%s\" failed.",
		     bundle, bundle->destination);
		bundle_forwarding_contraindicated(
			ctx,
			bundle,
			BUNDLE_SR_REASON_NO_KNOWN_ROUTE
		);
	}

	/* For steps after 5.4-2, see below */
	return UD3TN_OK;
}

/* 5.4-6 */
static void bundle_forwarding_success(
	const struct bp_context *const ctx, struct bundle *bundle)
{
	if (bundle->forwarding_refcount != 0)
		return;

	// We may transmit a bundle multiple times. If it failes one time and
	// this is not resolved by re-dispatch (the BDM is triggered again), it
	// is declared a forwarding fauilure.
	if (bundle->forwarding_failed) {
		LOGF_DEBUG(
			"BundleProcessor: Declaring forwarding failure for bundle %p, last transmission succeeded but a previous one failed",
			bundle
		);
		bundle_forwarding_failed(
			ctx,
			bundle,
			BUNDLE_SR_REASON_TRANSMISSION_CANCELED
		);
		return;
	}

	if (HAS_FLAG(bundle->proc_flags, BUNDLE_FLAG_REPORT_FORWARDING)) {
		/* See 5.4-6: reason code vs. unidirectional links */
		send_status_report(
			ctx,
			bundle,
			BUNDLE_SR_FLAG_BUNDLE_FORWARDED,
			BUNDLE_SR_REASON_NO_INFO
		);
	}
	bundle_rem_rc(bundle, BUNDLE_RET_CONSTRAINT_FORWARD_PENDING, 0);
	bundle_rem_rc(bundle, BUNDLE_RET_CONSTRAINT_FLAG_OWN, 1);
}

/* 5.4.1 */
static void bundle_forwarding_contraindicated(
	const struct bp_context *const ctx,
	struct bundle *bundle, enum bundle_status_report_reason reason)
{
	/* 5.4.1-1: For now, we declare forwarding failure everytime */
	bundle_forwarding_failed(ctx, bundle, reason);
	/* 5.4.1-2 (a): At the moment, custody transfer is declared as failed */
	/* 5.4.1-2 (b): Will not be handled */
}

/* 5.4.2 */
static void bundle_forwarding_failed(
	const struct bp_context *const ctx,
	struct bundle *bundle, enum bundle_status_report_reason reason)
{
	bundle->forwarding_failed = true;

	if (bundle->forwarding_refcount != 0) {
		LOGF_DEBUG(
			"BundleProcessor: Forwarding of bundle %p failed once but still enqueued for other peers",
			bundle
		);
		return;
	}

	LOGF_DEBUG(
		"BundleProcessor: Deleting bundle %p: Forwarding Failed",
		bundle
	);
	bundle_delete(ctx, bundle, reason);
}

/* 5.5 */
static void bundle_expired(
	const struct bp_context *const ctx, struct bundle *bundle)
{
	LOGF_DEBUG(
		"BundleProcessor: Deleting bundle %p: Lifetime Expired",
		bundle
	);
	bundle_delete(ctx, bundle, BUNDLE_SR_REASON_LIFETIME_EXPIRED);
}

/* 5.6 */
static void bundle_receive(struct bp_context *const ctx, struct bundle *bundle)
{
	struct bundle_block_list **e;
	enum bundle_handling_result res;

	// Set the reception time to calculate the bundle's residence time
	bundle->reception_timestamp_ms = hal_time_get_timestamp_ms();

	/* 5.6-1 Add retention constraint */
	bundle_add_rc(bundle, BUNDLE_RET_CONSTRAINT_DISPATCH_PENDING);
	/* 5.6-2 Request reception */
	if (HAS_FLAG(bundle->proc_flags, BUNDLE_FLAG_REPORT_RECEPTION))
		send_status_report(
			ctx,
			bundle,
			BUNDLE_SR_FLAG_BUNDLE_RECEIVED,
			BUNDLE_SR_REASON_NO_INFO
		);

	// Check lifetime
	const uint64_t timestamp_ms = hal_time_get_timestamp_ms();

	if (bundle_get_expiration_time_ms(bundle) < timestamp_ms) {
		bundle_expired(ctx, bundle);
		return;
	}

	/* 5.6-3 Handle blocks */
	e = &bundle->blocks;
	while (*e != NULL) {
		if ((*e)->data->type != BUNDLE_BLOCK_TYPE_PAYLOAD) {
			res = handle_unknown_block_flags(
				ctx,
				bundle,
				(*e)->data->flags
			);
			switch (res) {
			case BUNDLE_HRESULT_OK:
				(*e)->data->flags |=
					BUNDLE_V6_BLOCK_FLAG_FWD_UNPROC;
				break;
			case BUNDLE_HRESULT_DELETED:
				LOGF_DEBUG(
					"BundleProcessor: Deleting bundle %p: Block Unintelligible",
					bundle
				);
				bundle_delete(
					ctx,
					bundle,
					BUNDLE_SR_REASON_BLOCK_UNINTELLIGIBLE
				);
				return;
			case BUNDLE_HRESULT_BLOCK_DISCARDED:
				*e = bundle_block_entry_free(*e);
				break;
			}
		}
		if (*e != NULL)
			e = &(*e)->next;
	}

	/* NOTE: Test for custody acceptance here. */
	/* NOTE: We never accept custody, we do not have persistent storage. */

	/* 5.6-5 */
	bundle_dispatch(ctx, bundle);
}

/* 5.6-3 */
static enum bundle_handling_result handle_unknown_block_flags(
	const struct bp_context *const ctx,
	const struct bundle *bundle, enum bundle_block_flags flags)
{
	if (HAS_FLAG(flags, BUNDLE_BLOCK_FLAG_REPORT_IF_UNPROC)) {
		send_status_report(
			ctx,
			bundle,
			BUNDLE_SR_FLAG_BUNDLE_RECEIVED,
			BUNDLE_SR_REASON_BLOCK_UNINTELLIGIBLE
		);
	}
	if (HAS_FLAG(flags, BUNDLE_BLOCK_FLAG_DELETE_BUNDLE_IF_UNPROC))
		return BUNDLE_HRESULT_DELETED;
	else if (HAS_FLAG(flags, BUNDLE_BLOCK_FLAG_DISCARD_IF_UNPROC))
		return BUNDLE_HRESULT_BLOCK_DISCARDED;
	return BUNDLE_HRESULT_OK;
}

/* 5.7 */
static void bundle_deliver_local(
	struct bp_context *const ctx, struct bundle *bundle)
{
	bundle_rem_rc(bundle, BUNDLE_RET_CONSTRAINT_DISPATCH_PENDING, 0);

	/* Check and record knowledge of bundle */
	if (bundle_record_add_and_check_known(ctx, bundle)) {
		LOGF_DEBUG(
			"BundleProcessor: Bundle %p was already delivered, dropping.",
			bundle
		);
		// NOTE: We cannot have custody as the CM checks for duplicates
		bundle_discard(bundle);
		return;
	}

	/* Report successful delivery, if applicable */
	if (HAS_FLAG(bundle->proc_flags, BUNDLE_FLAG_REPORT_DELIVERY)) {
		send_status_report(
			ctx,
			bundle,
			BUNDLE_SR_FLAG_BUNDLE_DELIVERED,
			BUNDLE_SR_REASON_NO_INFO
		);
	}

	if (!HAS_FLAG(bundle->proc_flags, BUNDLE_FLAG_ADMINISTRATIVE_RECORD) &&
			get_agent_id(ctx, bundle->destination) == NULL) {
		// If it is no admin. record and we have no agent to deliver
		// it to, drop it.
		LOGF_DEBUG(
			"BundleProcessor: Received bundle not destined for any registered EID (from = %s, to = %s), dropping.",
			bundle->source,
			bundle->destination
		);
		bundle_delete(
			ctx,
			bundle,
			BUNDLE_SR_REASON_DEST_EID_UNINTELLIGIBLE
		);
		return;
	}

	if (HAS_FLAG(bundle->proc_flags, BUNDLE_FLAG_IS_FRAGMENT)) {
		bundle_add_rc(bundle, BUNDLE_RET_CONSTRAINT_REASSEMBLY_PENDING);
		bundle_attempt_reassembly(ctx, bundle);
	} else {
		struct bundle_adu adu = bundle_to_adu(bundle);

		bundle_discard(bundle);
		bundle_deliver_adu(ctx, adu);
	}
}

static bool may_reassemble(const struct bundle *b1, const struct bundle *b2)
{
	return (
		b1->creation_timestamp_ms == b2->creation_timestamp_ms &&
		b1->sequence_number == b2->sequence_number &&
		strcmp(b1->source, b2->source) == 0 // XXX: '==' may be ok
	);
}

static void add_to_reassembly_bundle_list(
	const struct bp_context *const ctx,
	struct reassembly_list *item,
	struct bundle *bundle)
{
	struct reassembly_bundle_list **cur_entry = &item->bundle_list;

	while (*cur_entry != NULL) {
		const struct reassembly_bundle_list *e = *cur_entry;

		// Order by frag. offset
		if (e->bundle->fragment_offset > bundle->fragment_offset)
			break;
		cur_entry = &(*cur_entry)->next;
	}

	struct reassembly_bundle_list *new_entry = malloc(
		sizeof(struct reassembly_bundle_list)
	);
	if (!new_entry) {
		LOGF_WARN(
			"BundleProcessor: Deleting bundle %p: Cannot store in reassembly list.",
			bundle
		);
		bundle_delete(ctx, bundle, BUNDLE_SR_REASON_DEPLETED_STORAGE);
		return;
	}
	new_entry->bundle = bundle;
	new_entry->next = *cur_entry;
	*cur_entry = new_entry;
}

static void try_reassemble(
	struct bp_context *const ctx, struct reassembly_list **slot)
{
	struct reassembly_list *const e = *slot;
	struct reassembly_bundle_list *eb;
	struct bundle *b;

	size_t pos_in_bundle = 0;

	LOG_DEBUG("BundleProcessor: Attempting bundle reassembly!");

	// Check if we can reassemble
	for (eb = e->bundle_list; eb; eb = eb->next) {
		b = eb->bundle;
		LOGF_DEBUG(
			"BundleProcessor: Evaluating fragment %p having size %lu and offset %lu for position %zu.",
			b,
			b->payload_block->length,
			b->fragment_offset,
			pos_in_bundle
		);
		if (b->fragment_offset > pos_in_bundle) {
			LOG_DEBUG("BundleProcessor: Reassembly not possible, gap detected.");
			return; // cannot reassemble, has gaps
		}
		pos_in_bundle = b->fragment_offset + b->payload_block->length;
		if (pos_in_bundle >= b->total_adu_length)
			break; // can reassemble
	}
	if (!eb)
		return;
	LOG_DEBUG("BundleProcessor: Reassembling bundle!");

	// Reassemble by memcpy
	b = e->bundle_list->bundle;
	const size_t adu_length = b->total_adu_length;
	uint8_t *const payload = malloc(adu_length);
	bool added_as_known = false;

	if (!payload) {
		LOG_WARN("BundleProcessor: Cannot allocate reassembly buffer!");
		return; // currently not enough memory to reassemble
	}

	struct bundle_adu adu = bundle_adu_init(b);

	adu.payload = payload;
	adu.length = adu_length;

	pos_in_bundle = 0;
	for (eb = e->bundle_list; eb; eb = eb->next) {
		b = eb->bundle;

		if (!added_as_known) {
			bundle_add_reassembled_as_known(ctx, b);
			added_as_known = true;
		}

		const size_t offset_in_bundle = (
			pos_in_bundle - b->fragment_offset
		);
		const size_t bytes_copied = MIN(
			b->payload_block->length - offset_in_bundle,
			adu_length - pos_in_bundle
		);

		if (offset_in_bundle < b->payload_block->length) {
			memcpy(
				&payload[pos_in_bundle],
				&b->payload_block->data[offset_in_bundle],
				bytes_copied
			);
			pos_in_bundle += bytes_copied;
		}

		bundle_rem_rc(b, BUNDLE_RET_CONSTRAINT_REASSEMBLY_PENDING, 0);
		bundle_discard(b);
	}

	// Delete slot
	*slot = (*slot)->next;
	while (e->bundle_list) {
		eb = e->bundle_list;
		e->bundle_list = e->bundle_list->next;
		free(eb);
	}
	free(e);

	// Deliver ADU
	bundle_deliver_adu(ctx, adu);
}

static void bundle_attempt_reassembly(
	struct bp_context *const ctx, struct bundle *bundle)
{
	struct reassembly_list **r_list_e = &ctx->reassembly_list;

	if (bundle_reassembled_is_known(ctx, bundle)) {
		LOGF_DEBUG(
			"BundleProcessor: Original bundle for %p was already delivered, dropping.",
			bundle
		);
		// Already delivered the original bundle
		bundle_rem_rc(
			bundle,
			BUNDLE_RET_CONSTRAINT_REASSEMBLY_PENDING,
			0
		);
		bundle_discard(bundle);
	}

	// Find bundle
	for (; *r_list_e; r_list_e = &(*r_list_e)->next) {
		struct reassembly_list *const e = *r_list_e;

		if (may_reassemble(e->bundle_list->bundle, bundle)) {
			LOGF_DEBUG(
				"BundleProcessor: Fragment list for new fragment %p found, updating.",
				bundle
			);
			add_to_reassembly_bundle_list(ctx, e, bundle);
			try_reassemble(ctx, r_list_e);
			return;
		}
	}

	// Not found, append
	struct reassembly_list *new_list = malloc(
		sizeof(struct reassembly_list)
	);

	if (!new_list) {
		LOGF_WARN(
			"BundleProcessor: Deleting bundle %p: Cannot create reassembly list.",
			bundle
		);
		bundle_delete(ctx, bundle, BUNDLE_SR_REASON_DEPLETED_STORAGE);
		return;
	}
	new_list->bundle_list = NULL;
	new_list->next = NULL;
	add_to_reassembly_bundle_list(ctx, new_list, bundle);
	*r_list_e = new_list;
	try_reassemble(ctx, r_list_e);
}

static void bundle_deliver_adu(const struct bp_context *const ctx, struct bundle_adu adu)
{
	struct bundle_administrative_record *record;

	if (HAS_FLAG(adu.proc_flags, BUNDLE_FLAG_ADMINISTRATIVE_RECORD)) {
		record = parse_administrative_record(
			adu.protocol_version,
			adu.payload,
			adu.length
		);

		if (record != NULL && record->type == BUNDLE_AR_CUSTODY_SIGNAL) {
			LOGF_DEBUG(
				"BundleProcessor: Received administrative record of type %u",
				record->type
			);
			bundle_handle_custody_signal(record);
			bundle_adu_free_members(adu);
		} else if (record != NULL &&
			   (record->type == BUNDLE_AR_BPDU ||
			    record->type == BUNDLE_AR_BPDU_COMPAT)) {
			ASSERT(record->start_of_record_ptr != NULL);
			ASSERT(record->start_of_record_ptr <
			       adu.payload + adu.length);
			const size_t bytes_to_skip = (
				record->start_of_record_ptr -
				adu.payload
			);

			// Remove the record-specific bytes from the ADU so
			// only the BPDU remains.
			adu.length = adu.length - bytes_to_skip;
			memmove(
				adu.payload,
				adu.payload + bytes_to_skip,
				adu.length
			);
			adu.proc_flags = BUNDLE_FLAG_ADMINISTRATIVE_RECORD;

			const char *agent_id = (
				get_eid_scheme(ctx->local_eid) == EID_SCHEME_DTN
				? "bibe"
				: "2925"
			);

			ASSERT(agent_id != NULL);
			LOGF_DEBUG(
				"BundleProcessor: Received BIBE bundle -> \"%s\"; len(PL) = %d B",
				agent_id,
				adu.length
			);
			agent_forward(ctx->agent_manager, agent_id, adu, ctx);
		} else if (record != NULL) {
			LOGF_DEBUG(
				"BundleProcessor: Received administrative record of unknown type %u, discarding.",
				record->type
			);
			bundle_adu_free_members(adu);
		} else {
			LOG_DEBUG("BundleProcessor: Received administrative record we cannot parse, discarding.");
			bundle_adu_free_members(adu);
		}

		free_administrative_record(record);
		return;
	}

	const char *agent_id = get_agent_id(ctx, adu.destination);

	ASSERT(agent_id != NULL);
	LOGF_DEBUG(
		"BundleProcessor: Received local bundle -> \"%s\"; len(PL) = %d B",
		agent_id,
		adu.length
	);
	agent_forward(ctx->agent_manager, agent_id, adu, ctx);
}

/* 5.13 (BPv7) */
/* NOTE: Custody Transfer Deferral would be implemented here. */

/* 5.13 (RFC 5050) */
/* 5.14 (BPv7-bis) */
static void bundle_delete(
	const struct bp_context *const ctx,
	struct bundle *bundle, enum bundle_status_report_reason reason)
{
	bool generate_report = false;

	/* NOTE: If custody was accepted, test this here and report. */

	if (HAS_FLAG(bundle->proc_flags, BUNDLE_FLAG_REPORT_DELETION))
		generate_report = true;

	if (generate_report)
		send_status_report(
			ctx,
			bundle,
			BUNDLE_SR_FLAG_BUNDLE_DELETED,
			reason
		);

	bundle->ret_constraints &= BUNDLE_RET_CONSTRAINT_NONE;
	bundle_discard(bundle);
}

/* 5.14 (RFC 5050) */
/* 5.15 (BPv7-bis) */
static void bundle_discard(struct bundle *bundle)
{
	LOGF_DEBUG("BundleProcessor: Discarding bundle %p", bundle);
	bundle_drop(bundle);
}

/* 6.3 */
static void bundle_handle_custody_signal(
	struct bundle_administrative_record *signal)
{
	/* NOTE: We never accept custody, we do not have persistent storage. */
	(void)signal;
}

/* HELPERS */

static void send_status_report(
	const struct bp_context *const ctx,
	const struct bundle *bundle,
	const enum bundle_status_report_status_flags status,
	const enum bundle_status_report_reason reason)
{
	if (!ctx->status_reporting)
		return;

	/* If the report-to EID is the null endpoint or uD3TN itself we do */
	/* not need to create a status report */
	if (bundle->report_to == NULL ||
	    strcmp(bundle->report_to, "dtn:none") == 0 ||
	    endpoint_is_local(ctx, bundle->report_to))
		return;

	struct bundle_status_report report = {
		.status = status,
		.reason = reason
	};
	struct bundle *b = generate_status_report(
		bundle,
		&report,
		ctx->local_eid
	);

	if (b != NULL) {
		bundle_add_rc(b, BUNDLE_RET_CONSTRAINT_DISPATCH_PENDING);
		if (bundle_forward(ctx, b) != UD3TN_OK)
			LOGF_DEBUG(
				"BundleProcessor: Failed sending status report for bundle %p.",
				bundle
			);
	}
}

static enum ud3tn_result enqueue_bundle_for_tx(
	QueueIdentifier_t tx_queue_handle,
	struct bundle *const bundle,
	const char *next_hop_node_id, const char *next_hop_cla_addr)
{
	LOGF_DEBUG(
		"BundleProcessor: Enqueuing bundle %p to \"%s\" for link with \"%s\".",
		bundle,
		bundle->destination,
		next_hop_node_id
	);

	struct cla_contact_tx_task_command command = {
		.type = TX_COMMAND_BUNDLE,
		.bundle = bundle,
	};

	command.node_id = strdup(next_hop_node_id);
	ASSERT(command.node_id);

	command.cla_address = strdup(next_hop_cla_addr);
	ASSERT(command.cla_address);

	bundle->forwarding_refcount++;

	const enum ud3tn_result result = hal_queue_try_push_to_back(
		tx_queue_handle,
		&command,
		BUNDLE_SEND_TIMEOUT_MS
	);

	if (result != UD3TN_OK)
		bundle->forwarding_refcount--;

	return result;
}

static enum ud3tn_result fragment_and_enqueue_bundle(
	QueueIdentifier_t tx_queue_handle,
	struct bundle *const bundle,
	const char *next_hop_node_id, const char *next_hop_cla_addr,
	const uint64_t mbs_bytes)
{
	ASSERT(mbs_bytes != 0);
	const uint64_t frag_min_sz_first = bundle_get_first_fragment_min_size(
		bundle
	);
	const uint64_t frag_min_sz_mid = bundle_get_mid_fragment_min_size(
		bundle
	);
	const uint64_t frag_min_sz_last = bundle_get_last_fragment_min_size(
		bundle
	);
	uint64_t cur_offset = 0;
	uint64_t remaining_payload = bundle->payload_block->length;
	uint64_t next_frag_min_size = frag_min_sz_first;

	while (remaining_payload > 0) {
		uint64_t frag_pay_sz;

		if (remaining_payload + next_frag_min_size <= mbs_bytes) {
			if (remaining_payload + frag_min_sz_last <= mbs_bytes)
				frag_pay_sz = remaining_payload;
			else
				frag_pay_sz = mbs_bytes - frag_min_sz_last;
		} else {
			frag_pay_sz = mbs_bytes - next_frag_min_size;
		}

		struct bundle *f = bundlefragmenter_fragment_bundle(
			bundle,
			cur_offset,
			frag_pay_sz
		);

		if (f == NULL) {
			LOGF_DEBUG(
				"BundleProcessor: Fragmentation failed for bundle %p to %s",
				bundle,
				next_hop_node_id
			);
			return UD3TN_FAIL;
		}

		LOGF_DEBUG(
			"BundleProcessor: Created fragment %p for bundle %p [%llu, %llu], forwarding to %s",
			f,
			bundle,
			cur_offset,
			cur_offset + frag_pay_sz,
			next_hop_node_id
		);

		// Enforce only one layer of nesting. We free it below.
		if (bundle->parent != NULL)
			f->parent = bundle->parent;
		else
			f->parent = bundle;

		const enum ud3tn_result rv = enqueue_bundle_for_tx(
			tx_queue_handle,
			f,
			next_hop_node_id,
			next_hop_cla_addr
		);

		// We cannot expect that the next calls work, so abort.
		if (rv != UD3TN_OK) {
			bundle_free(f);
			return UD3TN_FAIL;
		}

		// Increment the refcount for the original bundle (only done
		// for the fragment upon enqueuing).
		if (bundle->parent != NULL)
			bundle->parent->forwarding_refcount++;
		else
			bundle->forwarding_refcount++;

		next_frag_min_size = frag_min_sz_mid;
		remaining_payload -= frag_pay_sz;
		cur_offset += frag_pay_sz;
	}

	// The bundle already was a fragment. We only keep the top-level ref.
	if (bundle->parent != NULL)
		bundle_free(bundle);

	return UD3TN_OK;
}

static enum ud3tn_result send_bundle(
	const struct bp_context *const ctx, struct bundle *const bundle,
	const char *next_hop)
{
	struct fib_entry *entry = fib_lookup_node(ctx->fib, next_hop);

	if (entry == NULL) {
		LOGF_DEBUG(
			"BundleProcessor: No FIB entry for \"%s\"",
			next_hop
		);
		return UD3TN_FAIL;
	}

	const struct fib_link *link = fib_lookup_cla_addr(ctx->fib, entry->cla_addr);

	if (!link || link->status != FIB_LINK_STATUS_UP) {
		LOGF_DEBUG(
			"BundleProcessor: Link toward \"%s\" is inactive",
			next_hop
		);
		return UD3TN_FAIL;
	}

	ASSERT(entry->cla_addr != NULL);
	if (entry->cla_addr == NULL)
		return UD3TN_FAIL;
	// Try to obtain a handler
	struct cla_config *cla_config = cla_config_get(entry->cla_addr);

	if (!cla_config) {
		LOGF_DEBUG("BundleProcessor: Could not obtain CLA for address \"%s\"",
		     entry->cla_addr);
		return UD3TN_FAIL;
	}

	struct cla_tx_queue tx_queue = cla_config->vtable->cla_get_tx_queue(
		cla_config,
		next_hop,
		entry->cla_addr
	);

	if (!tx_queue.tx_queue_handle) {
		LOGF_DEBUG("BundleProcessor: Could not obtain queue for TX to \"%s\" via \"%s\"",
		     next_hop, entry->cla_addr);
		return UD3TN_FAIL;
	}

	const uint64_t cla_mbs_bytes = cla_config->vtable->cla_mbs_get(
		cla_config
	);
	const uint64_t bp_mbs_bytes = ctx->maximum_bundle_size_bytes;
	const uint64_t mbs_bytes = ((bp_mbs_bytes == 0) ? cla_mbs_bytes : (
		(cla_mbs_bytes == 0)
		? bp_mbs_bytes
		: MIN(bp_mbs_bytes, cla_mbs_bytes)
	));
	const uint64_t serialized_size_bytes = bundle_get_serialized_size(
		bundle
	);

	enum ud3tn_result result;

	if (mbs_bytes != 0 && serialized_size_bytes > mbs_bytes) {
		LOGF_DEBUG(
			"BundleProcessor: Bundle %p serialized size of %llu byte exceeds maximum bundle size of %llu byte",
			bundle,
			serialized_size_bytes,
			mbs_bytes
		);
		if (bundle_must_not_fragment(bundle)) {
			LOGF_DEBUG(
				"BundleProcessor: Bundle %p must not be fragmented - cannot be sent",
				bundle
			);
			result = UD3TN_FAIL;
		} else {
			result = fragment_and_enqueue_bundle(
				tx_queue.tx_queue_handle,
				bundle,
				next_hop,
				entry->cla_addr,
				mbs_bytes
			);
			// NOTE that if result is UD3TN_OK bundle may be freed
		}
	} else {
		result = enqueue_bundle_for_tx(
			tx_queue.tx_queue_handle,
			bundle,
			next_hop,
			entry->cla_addr
		);
	}

	hal_semaphore_release(tx_queue.tx_queue_sem); // taken by get_tx_queue

	if (result != UD3TN_OK)
		LOGF_DEBUG("BundleProcessor: Failed to send bundle %p", bundle);

	return result;
}

/**
 * 4.3.4. Hop Count (BPv7-bis)
 *
 * Checks if the hop limit exceeds the hop limit. If yes, the bundle gets
 * deleted and false is returned. Otherwise the hop count is incremented
 * and true is returned.
 *
 *
 * @return false if the hop count exeeds the hop limit, true otherwise
 */
static bool hop_count_validation(struct bundle *bundle)
{
	struct bundle_block *block = bundle_block_find_first_by_type(
		bundle->blocks, BUNDLE_BLOCK_TYPE_HOP_COUNT);

	/* No Hop Count block was found */
	if (block == NULL)
		return true;

	struct bundle_hop_count hop_count;
	bool success = bundle7_hop_count_parse(&hop_count,
		block->data, block->length);

	/* If block data cannot be parsed, ignore it */
	if (!success) {
		LOGF_DEBUG(
			"BundleProcessor: Could not parse hop-count block of bundle %p.",
			bundle
		);
		return true;
	}

	/* Hop count exceeded */
	if (hop_count.count >= hop_count.limit)
		return false;

	/* Increment Hop Count */
	hop_count.count++;

	/* CBOR-encoding */
	uint8_t *buffer = malloc(BUNDLE7_HOP_COUNT_MAX_ENCODED_SIZE);

	/* Out of memory - validation passes none the less */
	if (buffer == NULL) {
		LOGF_WARN(
			"BundleProcessor: Could not increment hop-count of bundle %p.",
			bundle
		);
		return true;
	}

	free(block->data);

	block->data = buffer;
	block->length = bundle7_hop_count_serialize(&hop_count,
		buffer, BUNDLE7_HOP_COUNT_MAX_ENCODED_SIZE);

	return true;
}

/**
 * Get the agent identifier for local bundle delivery.
 * The agent identifier should follow the local EID behind a slash ('/').
 */
static const char *get_agent_id(const struct bp_context *const ctx, const char *dest_eid)
{
	const size_t local_len = strlen(ctx->local_eid_prefix);
	const size_t dest_len = strlen(dest_eid);

	if (dest_len <= local_len)
		return NULL;
	// `ipn` EIDs always end with ".0", prefix ends with "."
	// -> agent starts after local_len
	if (ctx->local_eid_is_ipn) {
		if (dest_eid[local_len - 1] != '.')
			return NULL;
		return &dest_eid[local_len];
	}
	// The local `dtn` EID prefix never ends with a '/', it follows after
	// (see bundle_processor_task); the `<=` above protects this check
	if (dest_eid[local_len] != '/')
		return NULL;
	return &dest_eid[local_len + 1];
}

// Checks whether we know the bundle. If not, adds it to the list.
static bool bundle_record_add_and_check_known(
	struct bp_context *const ctx, const struct bundle *bundle)
{
	struct known_bundle_list **cur_entry = &ctx->known_bundle_list;
	uint64_t cur_time_ms = hal_time_get_timestamp_ms();
	const uint64_t bundle_deadline_ms = bundle_get_expiration_time_ms(
		bundle
	);

	if (bundle_deadline_ms < cur_time_ms)
		return true; // We assume we "know" all expired bundles.
	// 1. Cleanup and search
	while (*cur_entry != NULL) {
		struct known_bundle_list *e = *cur_entry;

		if (bundle_is_equal(bundle, &e->id)) {
			return true;
		} else if (e->deadline_ms < cur_time_ms) {
			*cur_entry = e->next;
			bundle_free_unique_identifier(&e->id);
			free(e);
			continue;
		} else if (e->deadline_ms > bundle_deadline_ms) {
			// Won't find, insert here!
			break;
		}
		cur_entry = &(*cur_entry)->next;
	}

	// 2. If not found, add at current slot (ordered by deadline)
	struct known_bundle_list *new_entry = malloc(
		sizeof(struct known_bundle_list)
	);

	if (!new_entry)
		return false;
	new_entry->id = bundle_get_unique_identifier(bundle);
	new_entry->deadline_ms = bundle_deadline_ms;
	new_entry->next = *cur_entry;
	*cur_entry = new_entry;

	return false;
}

static bool bundle_reassembled_is_known(
	struct bp_context *const ctx, const struct bundle *bundle)
{
	struct known_bundle_list **cur_entry = &ctx->known_bundle_list;
	const uint64_t bundle_deadline_ms = bundle_get_expiration_time_ms(
		bundle
	);

	while (*cur_entry != NULL) {
		struct known_bundle_list *e = *cur_entry;

		if (bundle_is_equal_parent(bundle, &e->id) &&
				e->id.fragment_offset == 0 &&
				e->id.payload_length ==
					bundle->total_adu_length) {
			return true;
		} else if (e->deadline_ms > bundle_deadline_ms) {
			// Won't find...
			break;
		}
		cur_entry = &(*cur_entry)->next;
	}
	return false;
}

static void bundle_add_reassembled_as_known(
	struct bp_context *const ctx, const struct bundle *bundle)
{
	struct known_bundle_list **cur_entry = &ctx->known_bundle_list;
	const uint64_t bundle_deadline_ms = bundle_get_expiration_time_ms(
		bundle
	);

	while (*cur_entry != NULL) {
		const struct known_bundle_list *e = *cur_entry;

		if (e->deadline_ms > bundle_deadline_ms)
			break;
		cur_entry = &(*cur_entry)->next;
	}

	struct known_bundle_list *new_entry = malloc(
		sizeof(struct known_bundle_list)
	);

	if (!new_entry)
		return;
	new_entry->id = bundle_get_unique_identifier(bundle);
	new_entry->id.fragment_offset = 0;
	new_entry->id.payload_length = bundle->total_adu_length;
	new_entry->deadline_ms = bundle_deadline_ms;
	new_entry->next = *cur_entry;
	*cur_entry = new_entry;
}
