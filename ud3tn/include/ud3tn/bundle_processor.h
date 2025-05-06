// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef BUNDLEPROCESSOR_H_INCLUDED
#define BUNDLEPROCESSOR_H_INCLUDED

#include "ud3tn/agent_manager.h"
#include "ud3tn/bundle.h"
#include "ud3tn/fib.h"

#include "platform/hal_types.h"

#include <stdbool.h>
#include <stdint.h>

// Blocks the BP if bundle cannot be enqueued -> 10 ms in worst case by default.
#ifndef BUNDLE_SEND_TIMEOUT_MS
#define BUNDLE_SEND_TIMEOUT_MS 10
#endif // BUNDLE_SEND_TIMEOUT_MS

// Maximum number of fragments that may be created by the BPA.
#ifndef BUNDLE_MAX_FRAGMENT_COUNT
#define BUNDLE_MAX_FRAGMENT_COUNT (1 << 16)
#endif // BUNDLE_MAX_FRAGMENT_COUNT

// Interface to the bundle agent, provided to other agents and the CLA.
struct bundle_agent_interface {
	char *local_eid;
	QueueIdentifier_t bundle_signaling_queue;
};

enum bundle_processor_signal_type {
	BP_SIGNAL_BUNDLE_INCOMING,
	BP_SIGNAL_TRANSMISSION_SUCCESS,
	BP_SIGNAL_TRANSMISSION_FAILURE,
	BP_SIGNAL_BUNDLE_LOCAL_DISPATCH,
	BP_SIGNAL_AGENT_REGISTER,
	BP_SIGNAL_AGENT_DEREGISTER,
	BP_SIGNAL_NEW_LINK_ESTABLISHED,
	BP_SIGNAL_LINK_DOWN,
	BP_SIGNAL_BUNDLE_BDM_DISPATCH,
	BP_SIGNAL_FIB_UPDATE_REQUEST,
};

// for performing (de)register operations
struct agent_manager_parameters {
	QueueIdentifier_t feedback_queue;
	struct agent agent;
};

enum fib_request_type {
	FIB_REQUEST_INVALID,
	// Create a new link and/or add an association node -> CLA address.
	FIB_REQUEST_CREATE_LINK,
	// Remove the FIB link and all node associations.
	FIB_REQUEST_DROP_LINK,
	// Only delete the node association. Note that there is no request to
	// add a node association -- this is done by CREATE_LINK
	FIB_REQUEST_DEL_NODE_ASSOC,
};

struct fib_request {
	char *node_id;
	char *cla_addr;
	enum fib_request_type type;
	enum fib_entry_flags flags;
};

struct bundle_processor_signal {
	enum bundle_processor_signal_type type;
	enum bundle_status_report_reason reason;
	struct bundle *bundle;
	char *peer_node_id;
	char *peer_cla_addr;
	struct agent_manager_parameters *agent_manager_params;
	struct dispatch_result *dispatch_result;
	struct fib_request *fib_request;
};

struct bundle_processor_task_parameters {
	QueueIdentifier_t signaling_queue;
	struct agent_manager *agent_manager;
	const char *local_eid;
	uint64_t maximum_bundle_size_bytes;
	bool status_reporting;
	bool allow_remote_configuration;
};

/**
 * Send the specified signal to the BP.
 *
 * @param bundle_processor_signaling_queue The BP signaling queue.
 * @param signal The signal to be sent.
 */
void bundle_processor_inform(
	QueueIdentifier_t bundle_processor_signaling_queue,
	const struct bundle_processor_signal signal);

/**
 * Instruct the BP to interact with the agent manager state.
 *
 * @param signaling_queue Handle to the signaling queue of
 *                        the BP task.
 * @param type BP_SIGNAL_AGENT_REGISTER or BP_SIGNAL_AGENT_DEREGISTER.
 * @param agent The agent parameters to be passed to the BP.
 *
 * @return The agent number (an internal identifier), or AGENT_NO_INVALID on
 *         error.
 */
agent_no_t bundle_processor_perform_agent_action(
	QueueIdentifier_t signaling_queue,
	enum bundle_processor_signal_type type,
	const struct agent agent);

/**
 * Instruct the BP to interact with the agent manager state asynchronously.
 *
 * @param signaling_queue Handle to the signaling queue of
 *                        the BP task.
 * @param type BP_SIGNAL_AGENT_REGISTER or BP_SIGNAL_AGENT_DEREGISTER.
 * @param agent The agent parameters to be passed to the BP.
 *
 * @return 0 when the signal was sent successfully, -1 on error.
 */
int bundle_processor_perform_agent_action_async(
	QueueIdentifier_t signaling_queue,
	enum bundle_processor_signal_type type,
	const struct agent agent);

// Forward declaration of internal opaque struct. Only to be used by agents
// from the BP task (not thread safe).
struct bp_context;

/**
 * Execute a bundle dispatch request. Note that this must only be executed from
 * the BP thread.
 *
 * @param bp_context The bundle processor state; needs to be passed-through.
 * @param bundle The bundle to be forwarded.
 * @param dispatch_result The BDM result, specifying the next-hop EIDs and
 *                        fragmentation parameters for the bundle.
 *
 * @note Only to be used by agents from the BP task (not thread safe).
 */
void bundle_processor_execute_bdm_dispatch(
	const struct bp_context *ctx, struct bundle *bundle,
	const struct dispatch_result *dispatch_result);

/**
 * Execute a FIB update request. Note that this must only be executed from the
 * BP thread.
 *
 * @param ctx The bundle processor state; needs to be passed-through.
 * @param fib_request The FIB update request to be processed.
 *
 * @note Only to be used by agents from the BP task (not thread safe).
 */
void bundle_processor_handle_fib_update_request(
	struct bp_context *const ctx,
	struct fib_request *fib_request);

/**
 * Dispatch a bundle. Note that this must only be executed from the BP thread.
 *
 * @param bp_context The bundle processor state; needs to be passed-through.
 * @param bundle The bundle to be dispatched.
 *
 * @note Only to be used by agents from the BP task (not thread safe).
 */
enum ud3tn_result bundle_processor_bundle_dispatch(
	void *bp_context, struct bundle *bundle);

/**
 * The main function of the BP task.
 *
 * @param param The BP task parameters (a pointer to a structure of type
 *              struct bundle_processor_task_parameters).
 */
void bundle_processor_task(void *param);

#endif /* BUNDLEPROCESSOR_H_INCLUDED */
