// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef RTCOMPAT_ROUTINGTABLE_H_INCLUDED
#define RTCOMPAT_ROUTINGTABLE_H_INCLUDED

#include "ud3tn/bundle.h"
#include "ud3tn/result.h"

#include "routing/compat/node.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Compat. router: Number of slots in the node hash table.
#ifndef ROUTER_NODE_HTAB_SLOT_COUNT
#define ROUTER_NODE_HTAB_SLOT_COUNT 128
#endif // ROUTER_NODE_HTAB_SLOT_COUNT

struct node_table_entry {
	uint16_t ref_count;
	struct contact_list *contacts;
};

typedef void (*reschedule_func_t)(
	void *context,
	struct bundle *bundle,
	uint64_t frag_offset,
	uint64_t frag_length
);
struct rescheduling_handle {
	reschedule_func_t reschedule_func;
	void *reschedule_func_context;
};

enum ud3tn_result routing_table_init(void);
void routing_table_free(void);

struct node *routing_table_lookup_node(const char *eid);
struct node_table_entry *routing_table_lookup_eid(const char *eid);

bool routing_table_add_node(
	struct node *new_node, struct rescheduling_handle rescheduler);
bool routing_table_replace_node(
	struct node *node, struct rescheduling_handle rescheduler);
bool routing_table_delete_node(
	struct node *new_node, struct rescheduling_handle rescheduler);
bool routing_table_delete_node_by_eid(
	const char *eid, struct rescheduling_handle rescheduler);

struct contact_list **routing_table_get_raw_contact_list_ptr(void);
struct node_list *routing_table_get_node_list(void);
void routing_table_delete_contact(struct contact *contact);
void routing_table_contact_passed(
	struct contact *contact, struct rescheduling_handle rescheduler);

#endif // RTCOMPAT_ROUTINGTABLE_H_INCLUDED
