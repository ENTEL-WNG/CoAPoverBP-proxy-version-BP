// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "routing/compat/config_parser.h"
#include "routing/compat/contact_manager.h"
#include "routing/compat/router_agent.h"

#include "ud3tn/agent_manager.h"
#include "ud3tn/bundle_processor.h"
#include "ud3tn/common.h"
#include "ud3tn/eid.h"

#include "platform/hal_io.h"
#include "platform/hal_queue.h"
#include "platform/hal_semaphore.h"
#include "platform/hal_time.h"
#include "platform/hal_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct config_parser parser;

struct router_agent_params {
	const char *aap2_admin_secret;
	bool allow_remote_configuration;
	struct contact_manager_params cm_param;
};

// BUNDLE HANDLING

#define BUNDLE_RESULT_NO_ROUTE 0
#define BUNDLE_RESULT_NO_TIMELY_CONTACTS -1
#define BUNDLE_RESULT_NO_MEMORY -2
#define BUNDLE_RESULT_INVALID -3
#define BUNDLE_RESULT_EXPIRED -4


static void hand_over_contact_bundles(
	struct contact *const contact,
	struct router_agent_params *const ra_param,
	const void *const bp_context)
{
	(void)ra_param;

	LOGF_DEBUG(
		"RouterAgent: Dispatching bundles for contact with \"%s\".",
		contact->node->eid
	);

	struct routed_bundle_list *rbl = contact->contact_bundles;

	contact->contact_bundles = NULL;

	while (rbl) {
		struct dispatch_next_hop dnh = {
			.node_id = contact->node->eid,
			.fragment_offset = rbl->frag_offset,
			.fragment_length = rbl->frag_length,
		};
		struct dispatch_result dr = {
			.orig_reason = DISPATCH_REASON_NO_FIB_ENTRY,
			.next_hop_count = 1,
			.next_hops = &dnh,
		};

		rbl->data->forwarding_refcount--;
		bundle_processor_execute_bdm_dispatch(
			bp_context,
			rbl->data,
			&dr
		);

		struct routed_bundle_list *const tmp = rbl;

		rbl = tmp->next;
		free(tmp);
	}
}

struct resched_func_param {
	const void *bp_context;
	struct router_agent_params *ra_param;
};

static void cra_bundle_resched_func(
	void *const param,
	struct bundle *bundle,
	uint64_t frag_offset,
	uint64_t frag_length)
{
	struct resched_func_param *const rfp = param;

	// NOTE that re-scheduling may be triggered by a dropped connection
	// if there is a scheduled contact but it apparently ended earlier.
	// In this case, we allow re-scheduling for the very same contact
	// as it will try to re-connect. However, if there is a better contact,
	// this one shall be used...

	struct router_result route = router_get_resched_route(
		bundle,
		frag_offset,
		frag_length
	);

	if (route.fragments == 0)
		goto resched_failed;

	ASSERT(route.fragment_results[0].payload_size == frag_length);

	const enum ud3tn_result ratc_rv = router_add_bundle_to_contact(
		route.fragment_results[0].contact,
		bundle,
		frag_offset,
		frag_length
	);

	if (ratc_rv != UD3TN_OK) {
		LOGF_WARN(
			"RouterAgent: Failed to add bundle %p to contact",
			bundle
		);
		goto resched_failed;
	}

	LOGF_DEBUG(
		"RouterAgent: Added re-scheduled bundle %p [%llu, %llu] to contact at %llu",
		bundle,
		frag_offset,
		frag_offset + frag_length,
		route.fragment_results[0].contact->from_ms
	);

	if (route.fragment_results[0].contact->link_active) {
		LOGF_DEBUG(
			"RouterAgent: Issuing immediate dispatch for contact at %llu",
			route.fragment_results[0].contact->from_ms
		);
		hand_over_contact_bundles(
			route.fragment_results[0].contact,
			rfp->ra_param,
			rfp->bp_context
		);
	}

	return;

resched_failed:
	LOGF_DEBUG(
		"RouterAgent: Bundle re-scheduling failed. Dropping bundle %p [%llu, %llu].",
		bundle,
		frag_offset,
		frag_offset + frag_length
	);

	// Decrement the refcount as we incremented it also for
	// the reference *we* kept (that was scheduled in the contact).
	bundle->forwarding_refcount--;

	struct dispatch_result dr = {
		.orig_reason = DISPATCH_REASON_TX_FAILED,
		.next_hop_count = 0,
		.next_hops = NULL,
	};

	// NOTE that this function is typically called with the
	// semaphore still held, so it must be ensured that no
	// transitive dispatch request is processed.
	bundle_processor_execute_bdm_dispatch(
		rfp->bp_context,
		bundle,
		&dr
	);
}

static void wake_up_contact_manager(QueueIdentifier_t cm_queue)
{
	int cm_signal = 1;

	// This never blocks and we ignore the return value: If there already
	// is something in the queue, the CM is about to be triggered, which is
	// what we want to achieve here.
	hal_queue_try_push_to_back(cm_queue, &cm_signal, 0);
}

static void rx_callback(
	struct bundle_adu data,
	void *const param, const void *const bp_context)
{
	struct router_agent_params *const ra_param = param;

	if (!ra_param->allow_remote_configuration && !data.bdm_auth_validated) {
		LOGF_DEBUG(
			"RouterAgent: Dropped unauthorized config message from \"%s\"",
			data.source
		);
		bundle_adu_free_members(data);
		return;
	}

	config_parser_read(
		&parser,
		data.payload,
		data.length
	);

	if (parser.basedata->status != PARSER_STATUS_DONE) {
		LOGF_DEBUG(
			"RouterAgent: Dropped invalid config message from \"%s\"",
			data.source
		);
		config_parser_reset(&parser);
		bundle_adu_free_members(data);
		return;
	}

	struct router_command *const cmd = parser.router_command;

	parser.router_command = NULL;
	config_parser_reset(&parser);
	bundle_adu_free_members(data);

	hal_semaphore_take_blocking(ra_param->cm_param.semaphore);

	bool success = true;
	const uint64_t cur_time_s = hal_time_get_timestamp_s();

	if (!node_prepare_and_verify(cmd->data, cur_time_s)) {
		free_node(cmd->data);
		LOGF_WARN("Router: Command (T = %c) is invalid!",
			cmd->type);
		free(cmd);
		return;
	}

	struct resched_func_param rfp = {
		.bp_context = bp_context,
		.ra_param = ra_param,
	};
	struct rescheduling_handle rescheduler = {
		.reschedule_func = cra_bundle_resched_func,
		.reschedule_func_context = &rfp,
	};

	switch (cmd->type) {
	case ROUTER_COMMAND_ADD:
		LOGF_INFO(
			"RouterAgent: Processing ADD command for node \"%s\"",
			cmd->data->eid
		);
		success = routing_table_add_node(
			cmd->data,
			rescheduler
		);
		break;
	case ROUTER_COMMAND_UPDATE:
		LOGF_INFO(
			"RouterAgent: Processing UPDATE command for node \"%s\"",
			cmd->data->eid
		);
		success = routing_table_replace_node(
			cmd->data,
			rescheduler
		);
		break;
	case ROUTER_COMMAND_DELETE:
		LOGF_INFO(
			"RouterAgent: Processing DELETE command for node \"%s\"",
			cmd->data->eid
		);
		success = routing_table_delete_node(
			cmd->data,
			rescheduler
		);
		break;
	default:
		free_node(cmd->data);
		success = false;
		break;
	}

	hal_semaphore_release(ra_param->cm_param.semaphore);

	if (success) {
		LOGF_DEBUG(
			"Router: Command (T = %c) processed.",
			cmd->type
		);
		wake_up_contact_manager(ra_param->cm_param.control_queue);
	} else {
		LOGF_INFO(
			"Router: Processing command (T = %c) failed!",
			cmd->type
		);
	}

	free(cmd);
}

static void fib_callback(
	const struct fib_entry *const entry,
	const char *node_id,
	enum fib_link_status link_status,
	void *param, const void *bp_context)
{
	struct router_agent_params *const ra_param = param;

	hal_semaphore_take_blocking(ra_param->cm_param.semaphore);

	const uint64_t cur_time_ms = hal_time_get_timestamp_ms();
	struct resched_func_param rfp = {
		.bp_context = bp_context,
		.ra_param = ra_param,
	};
	struct rescheduling_handle rescheduler = {
		.reschedule_func = cra_bundle_resched_func,
		.reschedule_func_context = &rfp,
	};
	struct node *node = routing_table_lookup_node(node_id);

	if (!node) {
		LOGF_DEBUG(
			"RouterAgent: Ignoring link update for unknown node ID \"%s\"",
			node_id
		);
		hal_semaphore_release(ra_param->cm_param.semaphore);
		return;
	}

	struct contact_list *cl = node->contacts;

	if (link_status == FIB_LINK_STATUS_UP) {
		// Determine the contact that became active and trigger the
		// bundle transmissions.
		while (cl) {
			if (cl->data->from_ms <= cur_time_ms &&
				cl->data->to_ms >= cur_time_ms
			) {
				cl->data->link_active = true;
				hand_over_contact_bundles(
					cl->data,
					ra_param,
					bp_context
				);
			} else if (cl->data->from_ms > cur_time_ms) {
				// The list is ordered - break here.
				break;
			}
			cl = cl->next;
		}
	} else if (link_status == FIB_LINK_STATUS_DOWN) {
		// For all contacts with the node that are still present but
		// expired, execute routing_table_contact_passed.
		while (cl) {
			if (cl->data->to_ms > cur_time_ms) {
				cl->data->link_active = false;
				// NOTE: This frees `cl`!
				routing_table_contact_passed(
					cl->data,
					rescheduler
				);
				// Re-init cl
				cl = node->contacts;
				continue;
			} else if (cl->data->from_ms > cur_time_ms) {
				// The list is ordered - break here.
				break;
			}
			cl = cl->next;
		}
	} else {
		LOGF_DEBUG(
			"RouterAgent: Ignoring link update with status %d",
			link_status
		);
	}
	hal_semaphore_release(ra_param->cm_param.semaphore);
}

static bool bdm_callback(
	struct bundle *const bundle,
	enum bundle_dispatch_reason reason,
	const char *orig_node_id,
	const char *orig_cla_addr,
	void *param, const void *bp_context)
{
	struct router_agent_params *const ra_param = param;

	// Other reasons are currently unsupported and we drop the bundle.
	if (reason != DISPATCH_REASON_NO_FIB_ENTRY)
		return false;

	(void)orig_node_id;
	(void)orig_cla_addr;
	(void)bp_context;

	ASSERT(bundle != NULL);
	if (!bundle)
		return false;

	const uint64_t timestamp_ms = hal_time_get_timestamp_ms();

	if (bundle_get_expiration_time_ms(bundle) < timestamp_ms) {
		// Bundle is already expired on arrival at the router...
		return false;
	}

	hal_semaphore_take_blocking(ra_param->cm_param.semaphore);

	struct router_result route = router_get_first_route(bundle);
	uint64_t cur_offset = 0;

	for (int i = 0; i < route.fragments; i++) {
		const enum ud3tn_result ratc_rv = router_add_bundle_to_contact(
			route.fragment_results[i].contact,
			bundle,
			cur_offset,
			route.fragment_results[i].payload_size
		);

		if (ratc_rv != UD3TN_OK) {
			// Remove the already-scheduled fragments again in case
			// of failure.
			for (int j = i - 1; j >= 0; j--) {
				cur_offset -= (
					route.fragment_results[j].payload_size
				);
				router_remove_bundle_from_contact(
					route.fragment_results[j].contact,
					bundle,
					cur_offset,
					route.fragment_results[j].payload_size
				);
				bundle->forwarding_refcount--;
			}
			LOGF_WARN("RouterAgent: Failed to add bundle %p to contact",
				  bundle);
			break;
		}

		bundle->forwarding_refcount++;

		LOGF_DEBUG(
			"RouterAgent: Added fragment of bundle %p [%llu, %llu] to contact at %llu",
			bundle,
			cur_offset,
			cur_offset + route.fragment_results[i].payload_size,
			route.fragment_results[i].contact->from_ms
		);

		if (route.fragment_results[i].contact->link_active) {
			LOGF_DEBUG(
				"RouterAgent: Issuing immediate dispatch for contact at %llu",
				route.fragment_results[i].contact->from_ms
			);
			hand_over_contact_bundles(
				route.fragment_results[i].contact,
				ra_param,
				bp_context
			);
		}

		cur_offset += route.fragment_results[i].payload_size;
	}

	hal_semaphore_release(ra_param->cm_param.semaphore);

	LOGF_DEBUG(
		"RouterAgent: Bundle %p [ %s ] [ frag = %d ]",
		bundle,
		(route.fragments < 1) ? "ERR" : "OK",
		route.fragments
	);

	return route.fragments != 0 ? true : false;
}


int compat_router_agent_setup(
	const struct bundle_agent_interface *bai,
	bool allow_remote_configuration,
	const char *aap2_admin_secret)
{
	const int is_ipn = get_eid_scheme(bai->local_eid) == EID_SCHEME_IPN;
	struct router_agent_params *const ra_param = malloc(
		sizeof(struct router_agent_params)
	);

	if (!ra_param) {
		LOG_ERROR("RouterAgent: Memory allocation failed!");
		return -1;
	}

	ra_param->aap2_admin_secret = aap2_admin_secret;
	ra_param->allow_remote_configuration = allow_remote_configuration;

	const struct parser *const config_parser_base = config_parser_init(&parser);

	if (!config_parser_base) {
		LOG_ERROR("RouterAgent: Could not initialize config parser!");
		free(ra_param);
		return -1;
	}

	if (routing_table_init() != UD3TN_OK) {
		LOG_ERROR("RouterAgent: Could not initialize routing table!");
		free(ra_param);
		return -1;
	}

	// NOTE: Compared to v0.13, the Contact Manager only starts and
	// terminates contacts (issuing the relevant FIB commands). Handing
	// over bundles to the BP for dispatch is done in response to the
	// FIB callback.
	ra_param->cm_param = contact_manager_start(
		bai->bundle_signaling_queue,
		routing_table_get_raw_contact_list_ptr()
	);

	if (ra_param->cm_param.task_creation_result != UD3TN_OK) {
		LOG_ERROR("RouterAgent: Failed to start Contact Manager!");
		free(ra_param);
		return -1;
	}

	const struct agent agent = {
		.auth_trx = true,
		.auth_fib = true,
		.auth_bdm = true,
		.is_subscriber = true,
		.sink_identifier = (
			is_ipn
			? ROUTER_AGENT_ID_CONFIG_IPN
			: ROUTER_AGENT_ID_CONFIG_DTN
		),
		.trx_callback = rx_callback,
		.fib_callback = fib_callback,
		.bdm_callback = bdm_callback,
		.param = ra_param,
		.secret = aap2_admin_secret,
	};
	const int rv = bundle_processor_perform_agent_action_async(
		bai->bundle_signaling_queue,
		BP_SIGNAL_AGENT_REGISTER,
		agent
	);

	if (rv) {
		LOG_ERROR("RouterAgent: BP failed to register SUB agent!");
		free(ra_param);
		config_parser_reset(&parser);
		return rv;
	}

	const struct agent agent_rpc = {
		.auth_trx = false,
		.auth_fib = true,
		.auth_bdm = true,
		.is_subscriber = false,
		.sink_identifier = (
			is_ipn
			? ROUTER_AGENT_ID_CONFIG_IPN
			: ROUTER_AGENT_ID_CONFIG_DTN
		),
		.secret = aap2_admin_secret,
	};
	const int rv_rpc = bundle_processor_perform_agent_action_async(
		bai->bundle_signaling_queue,
		BP_SIGNAL_AGENT_REGISTER,
		agent_rpc
	);

	if (rv_rpc) {
		LOG_ERROR("RouterAgent: BP failed to register RPC agent!");
		bundle_processor_perform_agent_action_async(
			bai->bundle_signaling_queue,
			BP_SIGNAL_AGENT_DEREGISTER,
			agent
		);
		free(ra_param);
		return rv_rpc;
	}

	return 0;
}
