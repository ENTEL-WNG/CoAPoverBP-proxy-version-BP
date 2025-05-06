// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef AGENT_MANAGER_H_INCLUDED
#define AGENT_MANAGER_H_INCLUDED

#include "ud3tn/bundle.h"
#include "ud3tn/fib.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The minimum length of the pre-shared administrative secret in case it is
// enabled. (In release builds this is enforced.)
#ifndef AAP2_MINIMUM_PRE_SHARED_SECRET_LENGTH
#define AAP2_MINIMUM_PRE_SHARED_SECRET_LENGTH 16
#endif // AAP2_MINIMUM_PRE_SHARED_SECRET_LENGTH

// See AAP 2.0 proto
enum bundle_dispatch_reason {
	DISPATCH_REASON_NO_FIB_ENTRY = 1,
	DISPATCH_REASON_LINK_INACTIVE = 2,
	DISPATCH_REASON_CLA_ENQUEUE_FAILED = 3,
	DISPATCH_REASON_TX_FAILED = 4,
	DISPATCH_REASON_TX_SUCCEEDED = 5,
};

struct dispatch_next_hop {
	char *node_id;
	uint64_t fragment_offset;
	uint64_t fragment_length;
};

struct dispatch_result {
	enum bundle_dispatch_reason orig_reason;
	int next_hop_count;
	struct dispatch_next_hop *next_hops;
};

/**
 * Type for the agent number, which is an internal identifier assigned by the
 * Agent Manager and must be specified on de-registration of the agent.
 * A value of zero indicates an invalid (or unset) agent number. It is used,
 * for example, to indicate the failure of actions returning an agent number.
 */
typedef uint64_t agent_no_t;
static const agent_no_t AGENT_NO_INVALID;

struct agent {
	// Whether or not the agent may send or receive bundles.
	bool auth_trx;
	// Whether or not the agent may send or receive FIB updates.
	bool auth_fib;
	// Whether or not the agent may dispatch bundles.
	bool auth_bdm;
	// Whether the agent is a subscriber or will initiate RPCs by itself.
	bool is_subscriber;

	// The secret to authorize agent registration: either a pre-shared
	// administrative secret or (for registering an agent in the other
	// direction of bundle forwarding) the same string as specified by the
	// other side.
	const char *secret;
	// The identifier of the bundle ADU sink under which the agent shall
	// be registered. The format is dependent on the EID scheme used.
	const char *sink_identifier;

	// A callback function for passing received ADUs to the agent.
	void (*trx_callback)(struct bundle_adu data, void *param,
			     const void *bp_context);
	// A callback function for passing FIB updates to the agent.
	void (*fib_callback)(const struct fib_entry *fib_entry,
			     const char *node_id,
			     enum fib_link_status link_status,
			     void *param, const void *bp_context);
	// A callback function for passing dispatch requests to the agent.
	bool (*bdm_callback)(struct bundle *bundle,
			     enum bundle_dispatch_reason reason,
			     const char *orig_node_id,
			     const char *orig_cla_addr,
			     void *param, const void *bp_context);
	// A callback function for checking if the agent connection still works.
	bool (*is_alive_callback)(void *param);

	// A user-defined parameter to be passed to the callback functions.
	void *param;

	// A number identifying the agent assigned by agent manager.
	uint64_t agent_no;
};

// Forward declaration of internal opaque struct. Not thread-safe.
struct agent_manager;

/**
 * Allocate and initialize the agent manager.
 *
 * @param ud3tn_nid The local node ID to be used by all agents.
 * @param adm_secret The pre-shared secret to be used for registering
 *                   agents that can modify the FIB or dispatch bundles.
 *                   NULL if no secret should be needed (prints a warning).
 *
 * @not_thread_safe
 */
struct agent_manager *agent_manager_create(const char *const local_nid,
					   const char *const adm_secret);

/**
 * Deallocate the specified agent manager.
 *
 * @param am A pointer to the agent manager.
 *
 * @not_thread_safe
 */
void agent_manager_free(struct agent_manager *am);

/**
 * Invoke the callback associated with the specified
 * sink_identifier in the thread of the caller.
 *
 * @param am A pointer to the agent manager.
 * @param sink_identifier The sink the ADU is addressed to.
 * @param data The bundle ADU to be forwarded.
 * @param bp_context The bundle processor context passed to the agent callback.
 *
 * @return The agent number, or zero in case an error was encountered
 *         (e.g., if the sink_identifier is unknown or the callback is NULL).
 *
 * @not_thread_safe
 */
agent_no_t agent_forward(struct agent_manager *am, const char *sink_identifier,
			 struct bundle_adu data, const void *bp_context);

/**
 * Invoke the callbacks associated with all agents subscribed for FIB updates
 * to pass the provided FIB entry.
 *
 * @param am A pointer to the agent manager.
 * @param fib_entry The FIB entry to be passed to the agent. If the data shall
 *                  persist after the execution of the callback, the agent must
 *                  create a copy explicitly.
 * @param node_id The node ID that the FIB entry is associated to, to be passed
 *                to the agent. If the data shall persist after the execution
 *                of the callback, the agent must create a copy explicitly.
 * @param link_status The current link status stored in the FIB.
 * @param bp_context The bundle processor context passed to the agent callback.
 *
 * @return 0 on success, or -1 on error (e.g., if no suitable agent was found).
 *
 * @not_thread_safe
 */
int agent_send_fib_info(struct agent_manager *am,
			const struct fib_entry *fib_entry,
			const char *node_id,
			enum fib_link_status link_status,
			const void *bp_context);

/**
 * Invoke the callback associated with the agent subscribed for bundle dispatch
 * requests to forward the provided dispatch request.
 *
 * @param am A pointer to the agent manager.
 * @param bundle The bundle to be dispatched. Any data or metadata that shall
 *               persist after the execution of the callback must be copied.
 * @param reason The reason associated with the dispatch request.
 * @param orig_node_id If set, the node ID that the bundle was previously
 *                     dispatched to, e.g., for indicating TX success/failure.
 * @param orig_cla_addr If set, the CLA address that the bundle was previously
 *                      dispatched to, e.g., for indicating TX success/failure.
 * @param bp_context The bundle processor context passed to the agent callback.
 *
 * @return 0 on success, or -1 on error (e.g., if no suitable agent was found
 *         or if the callback function returned false).
 *
 * @not_thread_safe
 */
int agent_handover_to_dispatch(struct agent_manager *am,
			       struct bundle *bundle,
			       enum bundle_dispatch_reason reason,
			       const char *orig_node_id,
			       const char *orig_cla_addr,
			       const void *bp_context);

/**
 * Register the specified agent.
 *
 * @param am A pointer to the agent manager.
 * @param agent The agent to be registered. Note that a shallow copy of the
 *              struct will be stored, thus, all pointers must be valid until
 *              the agent is de-registered.
 *
 * @return The assigned agent number, or zero if an error was encountered
 *         (e.g., if the sink_identifier is not unique or registration fails).
 *
 * @not_thread_safe
 */
agent_no_t agent_register(struct agent_manager *am, struct agent agent);

/**
 * Remove the agent associated with the specified agent number.
 *
 * @param am A pointer to the agent manager.
 * @param agent_no The agent number to be de-registered.
 *
 * @return The agent number, or zero in case an error was encountered (e.g.,
 *         if the agent was not found).
 *
 * @not_thread_safe
 */
agent_no_t agent_deregister(struct agent_manager *am, agent_no_t agent_no);

#endif /* AGENT_MANAGER_H_INCLUDED */
