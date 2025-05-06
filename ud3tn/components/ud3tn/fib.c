// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ud3tn/fib.h"
#include "ud3tn/simplehtab.h"
#include "ud3tn/common.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct fib {
	struct htab_entrylist *node_htab_elem[FIB_NODE_HTAB_SLOT_COUNT];
	struct htab node_htab;
	struct htab_entrylist *cla_addr_htab_elem[FIB_CLA_ADDR_HTAB_SLOT_COUNT];
	struct htab cla_addr_htab;

	bool htab_initialized;
};

struct fib *fib_alloc(void)
{
	struct fib *fib = malloc(sizeof(struct fib));

	if (!fib)
		return NULL;
	htab_init(
		&fib->node_htab,
		FIB_NODE_HTAB_SLOT_COUNT,
		fib->node_htab_elem
	);
	htab_init(
		&fib->cla_addr_htab,
		FIB_CLA_ADDR_HTAB_SLOT_COUNT,
		fib->cla_addr_htab_elem
	);

	return fib;
}

static void free_fib_entry(struct fib_entry *const entry)
{
	// We ensure that the CLA address is copied inside the FIB entry, thus,
	// we can safely discard the const qualifier here. It is intended to
	// protect the CLA address inside the FIB entry from changes by other
	// components that deal with fib_entry data structures.
	free((void *)entry->cla_addr);
	free(entry);
}

void fib_free(struct fib *const fib)
{
	// Free all values inside the nested htab entrylists.
	for (int i = 0; i < FIB_NODE_HTAB_SLOT_COUNT; i++) {
		struct htab_entrylist *el = fib->node_htab_elem[i];

		while (el) {
			struct fib_entry *entry = el->value;

			free_fib_entry(entry);
			el = el->next;
		}
	}
	for (int i = 0; i < FIB_CLA_ADDR_HTAB_SLOT_COUNT; i++) {
		struct htab_entrylist *el = fib->cla_addr_htab_elem[i];

		while (el) {
			struct fib_link *entry = el->value;

			free(entry);
			el = el->next;
		}
	}

	htab_trunc(&fib->node_htab);
	htab_trunc(&fib->cla_addr_htab);
	free(fib);
}

struct fib_entry *fib_lookup_node(struct fib *const fib,
				  const char *const node_id)
{
	return htab_get(&fib->node_htab, node_id);
}

struct fib_link *fib_lookup_cla_addr(struct fib *const fib,
				      const char *const cla_addr)
{
	return htab_get(&fib->cla_addr_htab, cla_addr);
}

struct fib_link *fib_insert_link(struct fib *const fib,
				 const char *const cla_addr)
{
	struct fib_link *entry = fib_lookup_cla_addr(fib, cla_addr);

	if (!entry) {
		entry = malloc(sizeof(struct fib_link));

		if (!entry)
			return NULL;

		entry->status = FIB_LINK_STATUS_UNSPECIFIED;
		entry->reflist = NULL;

		const struct htab_entrylist *rv = htab_add(
			&fib->cla_addr_htab,
			cla_addr,
			entry
		);

		if (!rv) {
			free(entry);
			return NULL;
		}

		entry->cla_addr = rv->key;
	}

	return entry;
}

static void fib_remove_node_ref(struct fib *const fib,
				struct fib_link *link_if_known,
				const char *const node_id,
				const char *const cla_addr)
{
	struct fib_link *link = (
		link_if_known ? link_if_known : fib_lookup_cla_addr(
			fib,
			cla_addr
		)
	);

	if (link) {
		struct fib_link_reflist **e = &link->reflist;

		while (*e) {
			if (strcmp((*e)->node_id, node_id) == 0) {
				struct fib_link_reflist *tmp = *e;

				*e = (*e)->next;
				free(tmp);
				break;
			}
			e = &(*e)->next;
		}
	}
}

struct fib_entry *fib_insert_node(struct fib *const fib,
				  const char *const node_id,
				  const char *const cla_addr)
{
	// First, ensure that a fib_link is present in the table.
	struct fib_link *link = fib_insert_link(fib, cla_addr);

	if (!link)
		return NULL;

	// Override if a mapping node_id -> cla_addr exists!
	struct fib_entry *prev_value = NULL;
	struct fib_entry *new_entry = malloc(sizeof(struct fib_entry));

	if (!new_entry)
		return NULL;

	new_entry->flags = FIB_ENTRY_FLAG_NONE;
	new_entry->cla_addr = strdup(cla_addr);

	if (!new_entry->cla_addr) {
		free(new_entry);
		return NULL;
	}

	struct fib_link_reflist *ref = malloc(sizeof(struct fib_link_reflist));

	if (!ref) {
		free_fib_entry(new_entry);
		return NULL;
	}

	const struct htab_entrylist *htab_entry = htab_update(
		&fib->node_htab, node_id, new_entry, (void **)&prev_value);

	if (!htab_entry) {
		free_fib_entry(new_entry);
		free(ref);
		return NULL;
	}

	if (prev_value) {
		fib_remove_node_ref(fib, NULL, node_id, prev_value->cla_addr);
		free_fib_entry(prev_value);
	}

	// Prepend to existing reflist.
	ref->node_id = htab_entry->key;
	ref->next = link->reflist;
	link->reflist = ref;

	return new_entry;
}

bool fib_remove_node(struct fib *const fib, const char *const node_id,
		     bool drop_all_associated)
{
	// NOTE: We cannot remove it, yet, as the key (the node_id char*) has
	// to stay valid -- it is referenced in the fib_link_reflist.
	struct fib_entry *entry = htab_get(&fib->node_htab, node_id);

	if (!entry)
		return false;

	struct fib_link *link = htab_get(&fib->cla_addr_htab, entry->cla_addr);

	ASSERT(link);
	if (!link)
		return false;

	if (drop_all_associated) {
		struct fib_link_reflist *e = link->reflist;

		while (e) {
			htab_remove(&fib->node_htab, e->node_id);
			struct fib_link_reflist *const tmp = e;

			e = e->next;
			free(tmp);
		}
		link->reflist = NULL;
	} else {
		fib_remove_node_ref(fib, link, node_id, entry->cla_addr);
	}

	if (!link->reflist && link->status != FIB_LINK_STATUS_UP)
		fib_remove_link(fib, entry->cla_addr);

	htab_remove(&fib->node_htab, node_id);
	free_fib_entry(entry);
	return true;
}

bool fib_remove_link(struct fib *const fib, const char *const cla_addr)
{
	struct fib_link *entry = htab_get(&fib->cla_addr_htab, cla_addr);

	if (!entry || entry->reflist)
		return false;

	htab_remove(&fib->cla_addr_htab, cla_addr);
	free(entry);
	return true;
}

void fib_foreach(struct fib *fib, fib_iter_fun *callback, void *context,
		 const char *cla_addr_filter)
{
	struct htab_entrylist *const *const node_entries = fib->node_htab_elem;
	const size_t node_slots = fib->node_htab.slot_count;

	// If we have a CLA address, we can use the reflist in the link struct.
	if (cla_addr_filter) {
		const struct fib_link *const link = fib_lookup_cla_addr(
			fib,
			cla_addr_filter
		);

		if (!link)
			return;

		const struct fib_link_reflist *refs = link->reflist;

		while (refs) {
			const struct fib_entry *const entry = fib_lookup_node(
				fib,
				refs->node_id
			);

			ASSERT(entry);
			if (!entry) {
				refs = refs->next;
				continue;
			}

			const bool rv = callback(
				context,
				refs->node_id,
				entry,
				link
			);

			if (!rv)
				return;
			refs = refs->next;
		}

		return;
	}

	for (size_t i = 0; i < node_slots; i++) {
		const struct htab_entrylist *el = node_entries[i];

		while (el) {
			const struct fib_entry *entry = el->value;
			// We support that `callback` calls `fib_remove_node`.
			const struct htab_entrylist *const next = el->next;
			const struct fib_link *const link = fib_lookup_cla_addr(
				fib,
				entry->cla_addr
			);

			ASSERT(link);
			if (!link) {
				el = next;
				continue;
			}

			const bool rv = callback(
				context,
				el->key,
				entry,
				link
			);

			if (!rv)
				return;
			el = next;
		}
	}
}
