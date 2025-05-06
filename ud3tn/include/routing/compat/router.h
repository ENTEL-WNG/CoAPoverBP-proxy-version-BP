// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef RTCOMPAT_ROUTER_H_INCLUDED
#define RTCOMPAT_ROUTER_H_INCLUDED

#include "platform/hal_types.h"

#include "ud3tn/bundle.h"
#include "ud3tn/common.h"

#include "routing/compat/contact_manager.h"
#include "routing/compat/node.h"
#include "routing/compat/routing_table.h"

#include <stddef.h>
#include <stdint.h>

// Compat. router: Maximum number of fragments created by the router.
#ifndef ROUTER_MAX_FRAGMENTS
#define ROUTER_MAX_FRAGMENTS 512
#endif // ROUTER_MAX_FRAGMENTS

// Compat. router: Default maximum bundle size.
#ifndef ROUTER_GLOBAL_MBS
#define ROUTER_GLOBAL_MBS SIZE_MAX
#endif // ROUTER_GLOBAL_MBS

// Compat. router: Default minimum payload for creating a fragment.
#ifndef ROUTER_FRAGMENT_MIN_PAYLOAD
#define ROUTER_FRAGMENT_MIN_PAYLOAD 8
#endif // ROUTER_FRAGMENT_MIN_PAYLOAD

struct router_config {
	size_t global_mbs;
	uint16_t fragment_min_payload;
};

struct fragment_route {
	uint64_t payload_size;
	struct contact *contact;
};

/* With MAX_FRAGMENTS = 3 and MAX_CONTACTS = 5: 92 bytes on stack */
struct router_result {
	struct fragment_route fragment_results[ROUTER_MAX_FRAGMENTS];
	int32_t fragments;
};

#define ROUTER_BUNDLE_PRIORITY(bundle) (bundle_get_routing_priority(bundle))
#define ROUTER_CONTACT_CAPACITY(contact, prio) \
	(contact_get_cur_remaining_capacity_bytes(contact, prio))

struct router_config router_get_config(void);
void router_update_config(struct router_config config);

struct contact_list *router_lookup_destination(const char *dest);
uint8_t router_calculate_fragment_route(
	struct fragment_route *res, uint64_t size,
	struct contact_list *contacts, uint64_t preprocessed_size,
	enum bundle_routing_priority priority, uint64_t exp_time,
	struct contact **excluded_contacts, uint8_t excluded_contacts_count);

struct router_result router_get_first_route(struct bundle *bundle);
struct router_result router_get_resched_route(
	struct bundle *bundle,
	const uint64_t frag_offset, const uint64_t frag_length);

enum ud3tn_result router_add_bundle_to_contact(
	struct contact *contact, struct bundle *b,
	uint64_t frag_offset, uint64_t frag_length);
enum ud3tn_result router_remove_bundle_from_contact(
	struct contact *contact, struct bundle *bundle,
	uint64_t frag_offset, uint64_t frag_length);

/* BP-side API */

enum router_command_type {
	ROUTER_COMMAND_UNDEFINED,
	ROUTER_COMMAND_ADD = 0x31,    /* ASCII 1 */
	ROUTER_COMMAND_UPDATE = 0x32, /* ASCII 2 */
	ROUTER_COMMAND_DELETE = 0x33, /* ASCII 3 */
	ROUTER_COMMAND_QUERY = 0x34   /* ASCII 4 */
};

struct router_command {
	enum router_command_type type;
	struct node *data;
};

#endif /* RTCOMPAT_ROUTER_H_INCLUDED */
