// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "ud3tn/agent_manager.h"
#include "ud3tn/bundle.h"
#include "ud3tn/common.h"
#include "ud3tn/eid.h"

#include "platform/hal_io.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct agent_list {
	struct agent agent_data;
	struct agent_list *next;
};

static struct agent_list **agent_search_ptr_by_no(struct agent_list **al_ptr,
						  agent_no_t agent_no);
static struct agent *agent_search_by_no(struct agent_list **al_ptr,
					agent_no_t agent_no);
static struct agent_list **agent_search_ptr_by_sink(
	struct agent_list **al_ptr, const char *sink_identifier);
static struct agent *agent_search_by_sink(struct agent_list **al_ptr,
					  const char *sink_identifier);
static int agent_list_add_entry(struct agent_list **al_ptr,
				struct agent obj);
static int agent_list_remove_entry_by_no(struct agent_list **al_ptr,
					 agent_no_t agent_no);
static int agent_drop_from_list(struct agent_list **const al,
				agent_no_t agent_no);

struct agent_manager {
	// Note that we use very simple data structures (linked lists) for now -
	// assuming a small number of concurrently-registered agents this
	// should be fine.
	struct agent_list *trx_sub_entry_node;
	struct agent_list *trx_rpc_entry_node;
	struct agent_list *fib_sub_entry_node;
	struct agent_list *fib_rpc_entry_node;
	struct agent_list *bdm_sub_entry_node;
	struct agent_list *bdm_rpc_entry_node;

	char *local_nid;
	enum eid_scheme local_eid_scheme;
	char *adm_secret;
	agent_no_t next_agent_no;
};

struct agent_manager *agent_manager_create(const char *const local_nid,
					   const char *const adm_secret)
{
	if (!local_nid || strlen(local_nid) <= 3 ||
	    get_eid_scheme(local_nid) == EID_SCHEME_UNKNOWN) {
		LOG_ERROR("AgentManager: Invalid local node ID specified, abort!");
		return NULL;
	}

	if (adm_secret == NULL) {
		LOG_WARN("AgentManager: No AAP 2.0 secret specified - all clients can dispatch and control the FIB!");
		if (!IS_DEBUG_BUILD) {
			LOG_ERROR("AgentManager: Insecure configuration, abort!");
			return NULL;
		}
	} else if (strlen(adm_secret) < AAP2_MINIMUM_PRE_SHARED_SECRET_LENGTH) {
		LOGF_WARN("AgentManager: AAP 2.0 secret shorter than %d characters - may be insecure!",
			  AAP2_MINIMUM_PRE_SHARED_SECRET_LENGTH);
		if (!IS_DEBUG_BUILD) {
			LOG_ERROR("AgentManager: Insecure configuration, abort!");
			return NULL;
		}
	}

	struct agent_manager *am = malloc(sizeof(struct agent_manager));

	if (!am) {
		LOG_ERROR("AgentManager: Allocation failed!");
		return NULL;
	}
	am->local_nid = strdup(local_nid);
	am->local_eid_scheme = get_eid_scheme(local_nid);
	am->adm_secret = adm_secret ? strdup(adm_secret) : NULL;
	if (!am->local_nid || (adm_secret && !am->adm_secret)) {
		LOG_ERROR("AgentManager: Allocation failed!");
		free(am->local_nid);
		free(am->adm_secret);
		free(am);
		return NULL;
	}
	am->trx_sub_entry_node = NULL;
	am->trx_rpc_entry_node = NULL;
	am->fib_sub_entry_node = NULL;
	am->fib_rpc_entry_node = NULL;
	am->bdm_sub_entry_node = NULL;
	am->bdm_rpc_entry_node = NULL;
	am->next_agent_no = 1;

	return am;
}

static void agent_list_free(struct agent_list *al)
{
	while (al) {
		struct agent_list *tmp = al;

		al = al->next;
		free(tmp);
	}
}

void agent_manager_free(struct agent_manager *am)
{
	if (!am)
		return;
	agent_list_free(am->trx_sub_entry_node);
	agent_list_free(am->trx_rpc_entry_node);
	agent_list_free(am->fib_sub_entry_node);
	agent_list_free(am->fib_rpc_entry_node);
	agent_list_free(am->bdm_sub_entry_node);
	agent_list_free(am->bdm_rpc_entry_node);
	free((void *)am->local_nid);
	free((void *)am->adm_secret);
	free(am);
}

static inline bool agent_has_sink_permission(
	const struct agent *registered_owner, struct agent agent)
{
	if (registered_owner->secret == NULL || agent.secret == NULL)
		return false;

	return (
		registered_owner->secret == agent.secret ||
		strcmp(registered_owner->secret, agent.secret) == 0
	);
}

static inline bool agent_has_adm_permission(const struct agent_manager *am,
					    struct agent agent,
					    bool allow_empty)
{
	if (am->adm_secret)
		return (
			agent.secret &&
			strcmp(agent.secret, am->adm_secret) == 0
		);

	// Only in debug builds, empty values are allowed.
	ASSERT(IS_DEBUG_BUILD);
	return allow_empty;
}

static int agent_register_trx(struct agent_manager *am, struct agent agent)
{
	struct agent_list **const al_ptr = (
		agent.is_subscriber
		? &am->trx_sub_entry_node
		: &am->trx_rpc_entry_node
	);
	struct agent_list **const al_ptr_other = (
		agent.is_subscriber
		? &am->trx_rpc_entry_node
		: &am->trx_sub_entry_node
	);

	if (am->local_eid_scheme == EID_SCHEME_IPN) {
		const char *end = parse_ipn_ull(agent.sink_identifier, NULL);

		if (end == NULL || *end != '\0') {
			LOG_WARN("AgentManager: Tried to register a sink with an invalid ipn service number!");
			return -1;
		}
	} else if (validate_dtn_eid_demux(agent.sink_identifier) != UD3TN_OK) {
		LOG_WARN("AgentManager: Tried to register a sink with an invalid dtn demux!");
		return -1;
	}

	/* check if agent with that sink_id is already existing */
	struct agent *const ag_conflict = agent_search_by_sink(
		al_ptr,
		agent.sink_identifier
	);

	if (ag_conflict != NULL) {
		if (ag_conflict->is_alive_callback != NULL &&
		    !ag_conflict->is_alive_callback(ag_conflict->param)) {
			LOGF_INFO(
				"AgentManager: Existing agent with sink_id %s reported a broken connection, dropping it.",
				agent.sink_identifier
			);
			agent_drop_from_list(al_ptr, ag_conflict->agent_no);
		} else {
			LOGF_WARN(
				"AgentManager: Agent with sink_id %s is already registered!",
				agent.sink_identifier
			);
			return -1;
		}
	}

	struct agent *const ag_ex = agent_search_by_sink(
		al_ptr_other,
		agent.sink_identifier
	);

	if (ag_ex && ag_ex->is_alive_callback != NULL &&
	    !ag_ex->is_alive_callback(ag_ex->param)) {
		LOGF_INFO(
			"AgentManager: Existing agent with sink_id %s reported a broken connection, dropping it.",
			agent.sink_identifier
		);
		agent_drop_from_list(al_ptr_other, ag_ex->agent_no);
	} else if (ag_ex &&
		   !agent_has_sink_permission(ag_ex, agent) &&
		   !agent_has_adm_permission(am, agent, false)) {
		LOGF_WARN(
			"AgentManager: Invalid secret provided for sink_id %s!",
			agent.sink_identifier
		);
		return -1;
	}

	if (agent_list_add_entry(al_ptr, agent)) {
		LOG_WARN("AgentManager: Error adding agent entry!");
		return -1;
	}

	LOGF_INFO(
		"AgentManager: Agent for %s ADUs registered for sink \"%s\" with no. %llu",
		agent.is_subscriber ? "receiving" : "sending",
		agent.sink_identifier,
		agent.agent_no
	);

	return 0;
}

static int agent_register_fib(struct agent_manager *am, struct agent agent)
{
	struct agent_list **const al_ptr = (
		agent.is_subscriber
		? &am->fib_sub_entry_node
		: &am->fib_rpc_entry_node
	);

	if (!agent_has_adm_permission(am, agent, true)) {
		LOG_WARN("AgentManager: Invalid secret provided for FIB access!");
		return -1;
	}

	if (agent_list_add_entry(al_ptr, agent)) // adding failed
		return -1;

	LOGF_INFO(
		"AgentManager: Agent for %s FIB entries registered with no. %llu",
		agent.is_subscriber ? "receiving" : "modifying",
		agent.agent_no
	);

	return 0;
}

static int agent_register_bdm(struct agent_manager *am, struct agent agent)
{
	struct agent_list **const al_ptr = (
		agent.is_subscriber
		? &am->bdm_sub_entry_node
		: &am->bdm_rpc_entry_node
	);

	if (agent.is_subscriber)
		ASSERT(agent.bdm_callback != NULL);

	if (!agent_has_adm_permission(am, agent, true)) {
		LOG_WARN("AgentManager: Invalid secret provided for BDM access!");
		return -1;
	}

	if (agent.is_subscriber && *al_ptr) {
		struct agent *const ag_conflict = (
			&(*al_ptr)->agent_data
		);

		if (ag_conflict->is_alive_callback != NULL &&
		    !ag_conflict->is_alive_callback(ag_conflict->param)) {
			LOG_INFO(
				"AgentManager: Existing agent for BDM commands reported a broken connection, dropping it."
			);
			agent_drop_from_list(
				al_ptr,
				ag_conflict->agent_no
			);
		} else {
			LOG_WARN("AgentManager: Only one subscribing BDM agent is allowed at the moment and one is already registered!");
			return -1;
		}
	}

	if (agent_list_add_entry(al_ptr, agent)) // adding failed
		return -1;

	LOGF_INFO(
		"AgentManager: Agent for %s registered with no. %llu",
		agent.is_subscriber ? "dispatching bundles" : "dispatch RPC",
		agent.agent_no
	);

	return 0;
}

agent_no_t agent_register(struct agent_manager *am, struct agent agent)
{
	if (!agent.auth_trx && !agent.auth_fib && !agent.auth_bdm) {
		LOG_WARN("AgentManager: Invalid request - no auth specified!");
		return AGENT_NO_INVALID;
	}

	agent.agent_no = am->next_agent_no;

	if (agent.auth_trx) {
		if (agent.is_subscriber && !agent.trx_callback) {
			LOG_WARN("AgentManager: Cannot register a TRX subscriber without TRX callback!");
			goto fail;
		}
		if (!agent.sink_identifier || agent.sink_identifier[0] == '\0') {
			LOG_WARN("AgentManager: Cannot register a TRX agent without a sink ID!");
			goto fail;
		}
		if (agent_register_trx(am, agent))
			goto fail;
	}

	if (agent.auth_fib) {
		if (agent.is_subscriber && !agent.fib_callback) {
			LOG_WARN("AgentManager: Cannot register a FIB subscriber without FIB callback!");
			goto fail;
		}
		if (agent_register_fib(am, agent))
			goto fail;
	}

	if (agent.auth_bdm) {
		if (agent.is_subscriber && !agent.bdm_callback) {
			LOG_WARN("AgentManager: Cannot register a BDM subscriber without BDM callback!");
			goto fail;
		}
		if (agent_register_bdm(am, agent))
			goto fail;
	}

	ASSERT(am->next_agent_no < UINT64_MAX);
	am->next_agent_no++;
	return agent.agent_no;

fail:
	agent_deregister(am, agent.agent_no);
	return AGENT_NO_INVALID;
}

agent_no_t agent_deregister(struct agent_manager *am, agent_no_t agent_no)
{
	int result = 0;

	result += !agent_drop_from_list(&am->trx_sub_entry_node, agent_no);
	result += !agent_drop_from_list(&am->trx_rpc_entry_node, agent_no);
	result += !agent_drop_from_list(&am->fib_sub_entry_node, agent_no);
	result += !agent_drop_from_list(&am->fib_rpc_entry_node, agent_no);
	result += !agent_drop_from_list(&am->bdm_sub_entry_node, agent_no);
	result += !agent_drop_from_list(&am->bdm_rpc_entry_node, agent_no);

	if (result == 0) {
		LOGF_DEBUG(
			"AgentManager: Agent with no. %llu was not registered -- nothing removed on requested de-registration.",
			agent_no
		);
		return AGENT_NO_INVALID;
	}

	return agent_no;
}

agent_no_t agent_forward(struct agent_manager *am, const char *sink_identifier,
			 struct bundle_adu data, const void *bp_context)
{
	struct agent *const ag_ptr = agent_search_by_sink(
		&am->trx_sub_entry_node,
		sink_identifier
	);

	if (ag_ptr == NULL) {
		LOGF_DEBUG(
			"AgentManager: No agent registered for identifier \"%s\"!",
			sink_identifier
		);
		bundle_adu_free_members(data);
		return AGENT_NO_INVALID;
	}

	if (ag_ptr->trx_callback == NULL) {
		LOGF_WARN(
			"AgentManager: Agent \"%s\" registered, but invalid (null) TRX callback function!",
			sink_identifier
		);
		bundle_adu_free_members(data);
		return AGENT_NO_INVALID;
	}
	ag_ptr->trx_callback(data, ag_ptr->param, bp_context);

	return ag_ptr->agent_no;
}

int agent_send_fib_info(struct agent_manager *am,
			const struct fib_entry *fib_entry,
			const char *node_id,
			enum fib_link_status link_status,
			const void *bp_context)
{
	struct agent_list *al = am->fib_sub_entry_node;

	if (!al) {
		LOG_DEBUG("AgentManager: No agent registered to send FIB infos!");
		return -1;
	}

	while (al) {
		ASSERT(al->agent_data.auth_fib);
		ASSERT(al->agent_data.fib_callback != NULL);

		if (al->agent_data.fib_callback != NULL)
			al->agent_data.fib_callback(
				fib_entry,
				node_id,
				link_status,
				al->agent_data.param,
				bp_context
			);
		else
			LOGF_WARN(
				"AgentManager: Agent no. %llu registered, but invalid (null) FIB callback function!",
				al->agent_data.agent_no
			);

		al = al->next;
	}

	return 0;
}

int agent_handover_to_dispatch(struct agent_manager *am,
			       struct bundle *bundle,
			       enum bundle_dispatch_reason reason,
			       const char *orig_node_id,
			       const char *orig_cla_addr,
			       const void *bp_context)
{
	if (!am->bdm_sub_entry_node) {
		LOG_DEBUG("AgentManager: No agent registered to dispatch!");
		return -1;
	}

	struct agent *ag_ptr = &am->bdm_sub_entry_node->agent_data;

	ASSERT(ag_ptr->auth_bdm);
	ASSERT(ag_ptr->bdm_callback != NULL);

	if (ag_ptr->bdm_callback != NULL)
		return (
			ag_ptr->bdm_callback(
				bundle,
				reason,
				orig_node_id,
				orig_cla_addr,
				ag_ptr->param,
				bp_context
			) ? 0 : -1
		);

	LOGF_WARN(
		"AgentManager: Agent no. %llu registered, but invalid (null) FIB callback function!",
		ag_ptr->agent_no
	);

	return -1;
}

// --- UTILITY FUNCTIONS ---

static struct agent_list **agent_search_ptr_by_no(struct agent_list **al_ptr,
						  agent_no_t agent_no)
{
	if (agent_no == AGENT_NO_INVALID)
		return NULL;
	/* loop runs until currently examined element is NULL => not found */
	while (*al_ptr) {
		if ((*al_ptr)->agent_data.agent_no == agent_no)
			return al_ptr;
		al_ptr = &(*al_ptr)->next;
	}
	return NULL;
}

static struct agent *agent_search_by_no(struct agent_list **al_ptr,
					agent_no_t agent_no)
{
	al_ptr = agent_search_ptr_by_no(al_ptr, agent_no);

	if (al_ptr)
		return &(*al_ptr)->agent_data;
	return NULL;
}

static struct agent_list **agent_search_ptr_by_sink(struct agent_list **al_ptr,
						    const char *sink_identifier)
{
	/* loop runs until currently examined element is NULL => not found */
	while (*al_ptr) {
		if (!strcmp((*al_ptr)->agent_data.sink_identifier,
			    sink_identifier))
			return al_ptr;
		al_ptr = &(*al_ptr)->next;
	}
	return NULL;
}

static struct agent *agent_search_by_sink(struct agent_list **al_ptr,
					  const char *sink_identifier)
{
	al_ptr = agent_search_ptr_by_sink(al_ptr, sink_identifier);

	if (al_ptr)
		return &(*al_ptr)->agent_data;
	return NULL;
}

static int agent_list_add_entry(struct agent_list **al_ptr, struct agent obj)
{
	struct agent_list *ag_ptr;
	struct agent_list *ag_iterator;

	if (obj.agent_no == AGENT_NO_INVALID)
		return -1; // invalid no., must not add
	if (agent_search_by_no(al_ptr, obj.agent_no) != NULL)
		return -1; // exists in list

	/* allocate the list struct and make sure next points to NULL */
	ag_ptr = malloc(sizeof(struct agent_list));
	if (ag_ptr == NULL)
		return -1;

	ag_ptr->next = NULL;
	ag_ptr->agent_data = obj;

	/* check if agent list is empty */
	if (*al_ptr == NULL) {
		/* make the new agent node the head of
		 * the list and return success
		 */
		*al_ptr = ag_ptr;
		return 0;
	}

	/* assign the root element to iterator */
	ag_iterator = *al_ptr;

	/* iterate over the linked list until the identifier is found
	 * or the end of the list is reached
	 */
	while (true) {
		if (ag_iterator->next != NULL) {
			ag_iterator = ag_iterator->next;
		} else {
			/* add list element to end of list
			 * (assuming that the list won't get very long,
			 *  additional ordering is not necessary and
			 *  considered an unnecessary overhead)
			 */
			ag_iterator->next = ag_ptr;
			return 0;
		}
	}
}

static int agent_list_remove_entry_by_no(struct agent_list **al_ptr,
					 agent_no_t agent_no)
{
	al_ptr = agent_search_ptr_by_no(al_ptr, agent_no);
	struct agent_list *next_element;

	if (!*al_ptr)
		/* entry not existing --> no removal necessary */
		return -1;

	/* perform the removal while keeping the proper pointer value */
	next_element = (*al_ptr)->next;
	free(*al_ptr);
	*al_ptr = next_element;
	return 0;
}

static int agent_drop_from_list(struct agent_list **const al,
				agent_no_t agent_no)
{
	const struct agent *ag_ptr = agent_search_by_no(
		al,
		agent_no
	);

	/* check if agent with that sink_id is not existing */
	if (ag_ptr == NULL)
		return -1;

	if (agent_list_remove_entry_by_no(al, agent_no))
		return -1;

	return 0;
}
