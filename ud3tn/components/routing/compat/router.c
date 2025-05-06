// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "routing/compat/node.h"
#include "routing/compat/router.h"
#include "routing/compat/routing_table.h"

#include "ud3tn/bundle.h"
#include "ud3tn/common.h"
#include "ud3tn/eid.h"

#include "cla/cla.h"

#include "platform/hal_io.h"
#include "platform/hal_time.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static struct router_config RC = {
	.global_mbs = ROUTER_GLOBAL_MBS,
	.fragment_min_payload = ROUTER_FRAGMENT_MIN_PAYLOAD,
};

struct router_config router_get_config(void)
{
	return RC;
}

void router_update_config(struct router_config conf)
{
	RC = conf;
}

struct contact_list *router_lookup_destination(const char *const dest)
{
	char *dest_node_eid = get_node_id(dest);
	const struct node_table_entry *e = NULL;

	if (dest_node_eid)
		e = routing_table_lookup_eid(dest_node_eid);

	// Fallback: perform a "dumb" string lookup
	if (!dest_node_eid || !e)
		e = routing_table_lookup_eid(dest);

	struct contact_list *result = NULL;

	if (e != NULL) {
		struct contact_list *cur = e->contacts;

		while (cur != NULL) {
			add_contact_to_ordered_list(&result, cur->data, 0);
			cur = cur->next;
		}
	}

	free(dest_node_eid);

	return result;
}

static inline struct max_fragment_size_result {
	uint32_t max_fragment_size;
	uint64_t payload_capacity;
} router_get_max_reasonable_fragment_size(
	struct contact_list *contacts, uint64_t full_size,
	uint64_t max_fragment_min_size, uint64_t payload_size,
	enum bundle_routing_priority priority, uint64_t exp_time)
{
	uint64_t payload_capacity = 0;
	uint32_t max_frag_size = UINT32_MAX;
	uint64_t min_capacity, c_capacity;
	int64_t c_pay_capacity;
	struct contact *c;

	(void)exp_time;
	min_capacity = payload_size / ROUTER_MAX_FRAGMENTS;
	min_capacity += max_fragment_min_size;
	while (contacts != NULL && payload_capacity < payload_size) {
		c = contacts->data;
		contacts = contacts->next;
		//if (c->to_s > exp_time)
		//	break;
		const int32_t rccap = ROUTER_CONTACT_CAPACITY(c, priority);

		// The return value of `contact_get_remaining_capacity_bytes`
		// is `INT32_MAX` is we assume an "infinite capacity" contact
		// As this determines the 64-bit payload capacity below and we
		// can use larger values here, we set the capacity value to
		// UINT64_MAX in this case.
		if (rccap == INT32_MAX)
			c_capacity = UINT64_MAX;
		else if (rccap < 0)
			c_capacity = 0;
		else
			c_capacity = rccap;
		// If size_t is smaller than uint64_t we need to cap the value.
		if (c_capacity > SIZE_MAX)
			c_capacity = SIZE_MAX;
		if (c_capacity < min_capacity)
			continue;

		// A CLA may have a maximum bundle size, determine it
		struct cla_config *const cla_config = cla_config_get(
			c->node->cla_addr
		);

		if (!cla_config)
			continue;

		const uint64_t cla_mbs_bytes = cla_config->vtable->cla_mbs_get(
			cla_config
		);
		const uint64_t bp_mbs_bytes = RC.global_mbs;
		const uint64_t mbs_bytes = (
			(bp_mbs_bytes == 0) ? cla_mbs_bytes : (
				(cla_mbs_bytes == 0)
				? bp_mbs_bytes
				: MIN(bp_mbs_bytes, cla_mbs_bytes)
			)
		);
		const size_t c_mbs = mbs_bytes == 0 ? (size_t)c_capacity : MIN(
			(size_t)c_capacity,
			mbs_bytes
		);

		// Contact of "infinite" capacity -> max. frag. size == MBS
		if (c_capacity >= INT32_MAX) {
			const uint32_t max_fragment_size = MIN(
				(uint32_t)INT32_MAX,
				c_mbs
			);

			return (struct max_fragment_size_result){
				max_fragment_size,
				payload_size,
			};
		}

		c_pay_capacity = c_capacity - max_fragment_min_size;
		if (c_pay_capacity > RC.fragment_min_payload) {
			payload_capacity += c_pay_capacity;
			max_frag_size = MIN(max_frag_size, c_mbs);
			if (c_capacity >= full_size)
				break;
		}
	}

	if (max_frag_size >= INT32_MAX)
		max_frag_size = INT32_MAX; // "infinite"

	return (struct max_fragment_size_result){
		payload_capacity < payload_size ? 0 : max_frag_size,
		payload_capacity,
	};
}

uint8_t router_calculate_fragment_route(
	struct fragment_route *res, uint64_t size,
	struct contact_list *contacts, uint64_t preprocessed_size,
	enum bundle_routing_priority priority, uint64_t exp_time_ms,
	struct contact **excluded_contacts, uint8_t excluded_contacts_count)
{
	const uint64_t time_ms = hal_time_get_timestamp_ms();

	(void)priority;

	res->contact = NULL;
	while (contacts != NULL) {
		struct contact *c = contacts->data;
		uint8_t d = 0;

		contacts = contacts->next;

		for (uint8_t i = 0; i < excluded_contacts_count; i++)
			if (c == excluded_contacts[i])
				d = 1;
		if (d)
			continue;
		if (c->from_ms >= exp_time_ms)
			continue; // Ignore -- NOTE: List is ordered by c->to
		if (c->to_ms <= time_ms)
			continue;
		uint64_t cap = ROUTER_CONTACT_CAPACITY(c, 0);
		// "infinite" capacity contact
		if (cap == INT32_MAX)
			cap = UINT64_MAX;
		if (preprocessed_size != 0) {
			if (preprocessed_size >= cap) {
				preprocessed_size -= cap;
				continue;
			} else {
				cap -= preprocessed_size;
				/* Don't set to zero here, needed in next if */
			}
		}
		if (cap < size) {
			preprocessed_size = 0;
			continue;
		}
		res->contact = c;
		break;
	}
	return (res->contact != NULL);
}

static inline void router_get_first_route_nonfrag(
	struct router_result *res, struct contact_list *contacts,
	struct bundle *bundle, uint64_t bundle_size,
	uint64_t expiration_time_ms)
{
	res->fragment_results[0].payload_size = bundle->payload_block->length;
	/* Determine route */
	if (router_calculate_fragment_route(
		&res->fragment_results[0], bundle_size,
		contacts, 0, ROUTER_BUNDLE_PRIORITY(bundle), expiration_time_ms,
		NULL, 0)
	)
		res->fragments = 1;
}

static inline void router_get_first_route_frag(
	struct router_result *res, struct contact_list *contacts,
	struct bundle *bundle, uint64_t bundle_size,
	uint64_t expiration_time_ms,
	uint64_t max_frag_sz, uint64_t first_frag_sz, uint64_t last_frag_sz)
{
	/* Determine fragment minimum sizes */
	const uint64_t mid_frag_sz = bundle_get_mid_fragment_min_size(bundle);
	uint64_t next_frag_sz = first_frag_sz;

	if (next_frag_sz > max_frag_sz || last_frag_sz > max_frag_sz) {
		LOGF_DEBUG(
			"Router: Cannot fragment because max. frag. size of %llu bytes is smaller than bundle headers (first = %llu, mid = %llu, last = %llu)",
			max_frag_sz,
			next_frag_sz,
			mid_frag_sz,
			last_frag_sz
		);
		return; /* failed */
	}

	uint64_t remaining_pay = bundle->payload_block->length;

	while (remaining_pay != 0 && res->fragments < ROUTER_MAX_FRAGMENTS) {
		const int64_t min_pay = MIN(
			remaining_pay,
			RC.fragment_min_payload
		);
		int64_t max_pay = max_frag_sz - next_frag_sz;

		if (max_pay < min_pay) {
			LOGF_DEBUG(
				"Router: Cannot fragment because minimum amount of payload (%llu bytes) will not fit in fragment with maximum payload size of %llu bytes",
				min_pay,
				max_pay
			);
			break; /* failed: remaining_pay != 0 */
		}
		if (remaining_pay <= max_frag_sz - last_frag_sz) {
			/* Last fragment */
			res->fragment_results[res->fragments++].payload_size = remaining_pay;
			remaining_pay = 0;
		} else {
			/* Another fragment */
			max_pay = MIN((int64_t)remaining_pay, max_pay);
			res->fragment_results[res->fragments++].payload_size = max_pay;
			remaining_pay -= max_pay;
			next_frag_sz = mid_frag_sz;
		}
	}
	if (remaining_pay != 0) {
		res->fragments = 0;
		return; /* failed */
	}

	/* Determine routes */
	int64_t success = 0;
	uint64_t processed_sz = 0;

	for (int64_t index = 0; index < res->fragments; index++) {
		bundle_size = res->fragment_results[index].payload_size;
		if (index == 0)
			bundle_size += first_frag_sz;
		else if (index == res->fragments - 1)
			bundle_size += last_frag_sz;
		else
			bundle_size += mid_frag_sz;
		success += router_calculate_fragment_route(
			&res->fragment_results[index], bundle_size,
			contacts, processed_sz, ROUTER_BUNDLE_PRIORITY(bundle),
			expiration_time_ms, NULL, 0);
		processed_sz += bundle_size;
	}
	if (success != res->fragments)
		res->fragments = 0;
}

struct router_result router_get_first_route(struct bundle *bundle)
{
	const uint64_t expiration_time_ms = bundle_get_expiration_time_ms(
		bundle
	);
	struct router_result res;
	struct contact_list *contacts = router_lookup_destination(
		bundle->destination
	);

	res.fragments = 0;
	if (contacts == NULL) {
		LOGF_DEBUG(
			"Router: Could not determine a node over which the destination \"%s\" for bundle %p is reachable",
			bundle->destination,
			bundle
		);
		return res;
	}

	const uint64_t bundle_size = bundle_get_serialized_size(bundle);
	const uint64_t first_frag_sz = bundle_get_first_fragment_min_size(
		bundle
	);
	const uint64_t last_frag_sz = bundle_get_last_fragment_min_size(bundle);

	const struct max_fragment_size_result mrfs =
		router_get_max_reasonable_fragment_size(
			contacts,
			bundle_size,
			MAX(first_frag_sz, last_frag_sz),
			bundle->payload_block->length,
			ROUTER_BUNDLE_PRIORITY(bundle),
			expiration_time_ms
		);

	if (mrfs.max_fragment_size == 0) {
		LOGF_DEBUG(
			"Router: Contact payload capacity (%llu bytes) too low for bundle %p of size %llu bytes (min. frag. sz. = %llu, payload sz. = %llu)",
			mrfs.payload_capacity,
			bundle,
			bundle_size,
			MAX(first_frag_sz, last_frag_sz),
			bundle->payload_block->length
		);
		goto finish;
	} else if (mrfs.max_fragment_size != INT32_MAX) {
		LOGF_DEBUG(
			"Router: Determined max. frag size of %lu bytes for bundle %p of size %llu bytes (payload sz. = %llu)",
			mrfs.max_fragment_size,
			bundle,
			bundle_size,
			bundle->payload_block->length
		);
	} else {
		LOGF_DEBUG(
			"Router: Determined infinite max. frag size for bundle of size %llu bytes (payload sz. = %llu)",
			bundle_size,
			bundle->payload_block->length
		);
	}

	if (bundle_must_not_fragment(bundle) ||
			bundle_size <= mrfs.max_fragment_size)
		router_get_first_route_nonfrag(&res,
			contacts, bundle, bundle_size, expiration_time_ms);
	else
		router_get_first_route_frag(&res,
			contacts, bundle, bundle_size, expiration_time_ms,
			mrfs.max_fragment_size, first_frag_sz, last_frag_sz);

	if (!res.fragments)
		LOGF_DEBUG(
			"Router: No feasible route found for bundle %p to \"%s\" with size of %llu bytes",
			bundle,
			bundle->destination,
			bundle_size
		);

finish:
	while (contacts) {
		struct contact_list *const tmp = contacts->next;

		free(contacts);
		contacts = tmp;
	}
	return res;
}

static size_t get_serialized_size_frag(
	struct bundle *b,
	uint64_t frag_offset, uint64_t frag_length)
{
	uint64_t header_size;

	// Would be a bug - fragment would end after end of bundle.
	ASSERT(frag_offset + frag_length <= b->payload_block->length);

	if (frag_offset == 0)
		header_size = bundle_get_first_fragment_min_size(b);
	else if (frag_offset + frag_length == b->payload_block->length)
		header_size = bundle_get_last_fragment_min_size(b);
	else
		header_size = bundle_get_mid_fragment_min_size(b);

	return header_size + frag_length;
}

struct router_result router_get_resched_route(
	struct bundle *bundle,
	const uint64_t frag_offset, const uint64_t frag_length)
{
	const uint64_t expiration_time_ms = bundle_get_expiration_time_ms(
		bundle
	);
	struct router_result res;
	struct contact_list *contacts = router_lookup_destination(
		bundle->destination
	);

	res.fragments = 0;
	if (contacts == NULL) {
		LOGF_DEBUG(
			"Router: Could not determine a node over which the destination \"%s\" for re-scheduling bundle %p [%llu, %llu] is reachable",
			bundle->destination,
			bundle,
			frag_offset,
			frag_offset + frag_length
		);
		return res;
	}

	const uint64_t bundle_size = get_serialized_size_frag(
		bundle,
		frag_offset,
		frag_length
	);

	const struct max_fragment_size_result mrfs =
		router_get_max_reasonable_fragment_size(
			contacts,
			bundle_size,
			bundle_size - frag_length,
			frag_length,
			ROUTER_BUNDLE_PRIORITY(bundle),
			expiration_time_ms
		);

	if (mrfs.max_fragment_size == 0) {
		LOGF_DEBUG(
			"Router: Contact payload capacity (%llu bytes) too low for re-scheduling bundle %p [%llu, %llu] of size %llu bytes",
			mrfs.payload_capacity,
			bundle,
			frag_offset,
			frag_offset + frag_length,
			bundle_size
		);
		goto finish;
	} else if (mrfs.max_fragment_size != INT32_MAX) {
		LOGF_DEBUG(
			"Router: Determined max. frag size of %lu bytes for re-scheduling bundle %p [%llu, %llu] of size %llu bytes",
			mrfs.max_fragment_size,
			bundle,
			frag_offset,
			frag_offset + frag_length,
			bundle_size
		);
	} else {
		LOGF_DEBUG(
			"Router: Determined infinite max. frag size for re-scheduling bundle %p [%llu, %llu] of size %llu bytes",
			bundle,
			frag_offset,
			frag_offset + frag_length,
			bundle_size
		);
	}

	router_get_first_route_nonfrag(
		&res,
		contacts,
		bundle,
		bundle_size,
		expiration_time_ms
	);

	if (!res.fragments)
		LOGF_DEBUG(
			"Router: No feasible route found for re-scheduling bundle %p [%llu, %llu] to \"%s\" with size of %llu bytes",
			bundle,
			frag_offset,
			frag_offset + frag_length,
			bundle->destination,
			bundle_size
		);

finish:
	while (contacts) {
		struct contact_list *const tmp = contacts->next;

		free(contacts);
		contacts = tmp;
	}
	return res;
}

enum ud3tn_result router_add_bundle_to_contact(
	struct contact *contact, struct bundle *b,
	uint64_t frag_offset, uint64_t frag_length)
{
	struct routed_bundle_list *new_entry, **cur_entry;

	ASSERT(contact != NULL);
	ASSERT(b != NULL);
	if (!contact || !b)
		return UD3TN_FAIL;
	ASSERT(contact->remaining_capacity_p0 > 0);

	new_entry = malloc(sizeof(struct routed_bundle_list));
	if (new_entry == NULL)
		return UD3TN_FAIL;
	new_entry->data = b;
	new_entry->frag_offset = frag_offset;
	new_entry->frag_length = frag_length;
	new_entry->next = NULL;
	cur_entry = &contact->contact_bundles;
	/* Go to end of list (=> FIFO) */
	while (*cur_entry != NULL) {
		ASSERT((*cur_entry)->data != b);
		if ((*cur_entry)->data == b) {
			free(new_entry);
			return UD3TN_FAIL;
		}
		cur_entry = &(*cur_entry)->next;
	}
	*cur_entry = new_entry;
	// This contact is of infinite capacity, just return "OK".
	if (contact->remaining_capacity_p0 == INT32_MAX)
		return UD3TN_OK;

	const size_t bundle_size = get_serialized_size_frag(
		b,
		frag_offset,
		frag_length
	);
	const enum bundle_routing_priority prio =
		bundle_get_routing_priority(b);

	contact->remaining_capacity_p0 -= bundle_size;
	if (prio > BUNDLE_RPRIO_LOW) {
		contact->remaining_capacity_p1 -= bundle_size;
		if (prio != BUNDLE_RPRIO_NORMAL)
			contact->remaining_capacity_p2 -= bundle_size;
	}
	return UD3TN_OK;
}

enum ud3tn_result router_remove_bundle_from_contact(
	struct contact *contact, struct bundle *bundle,
	uint64_t frag_offset, uint64_t frag_length)
{
	struct routed_bundle_list **cur_entry, *tmp;

	ASSERT(contact != NULL);
	if (!contact)
		return UD3TN_FAIL;
	cur_entry = &contact->contact_bundles;
	/* Find bundle */
	while (*cur_entry != NULL) {
		ASSERT((*cur_entry)->data != NULL);
		if ((*cur_entry)->data && (*cur_entry)->data == bundle &&
			(*cur_entry)->frag_offset == frag_offset &&
			(*cur_entry)->frag_length == frag_length
		) {
			tmp = *cur_entry;
			*cur_entry = (*cur_entry)->next;
			free(tmp);
			// This contact is of infinite capacity, do nothing.
			if (contact->remaining_capacity_p0 == INT32_MAX)
				continue;

			const size_t bundle_size = get_serialized_size_frag(
				bundle,
				frag_offset,
				frag_length
			);
			const enum bundle_routing_priority prio =
				bundle_get_routing_priority(bundle);

			contact->remaining_capacity_p0 += bundle_size;
			if (prio > BUNDLE_RPRIO_LOW) {
				contact->remaining_capacity_p1 += bundle_size;
				if (prio != BUNDLE_RPRIO_NORMAL)
					contact->remaining_capacity_p2 += bundle_size;
			}
			return UD3TN_OK;
		}
		cur_entry = &(*cur_entry)->next;
	}
	return UD3TN_FAIL;
}
