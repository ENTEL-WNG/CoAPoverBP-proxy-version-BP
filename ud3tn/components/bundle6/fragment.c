// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "bundle6/fragment.h"

#include "ud3tn/bundle.h"
#include "ud3tn/common.h"
#include "ud3tn/bundle_fragmenter.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


struct bundle *bundle6_fragment_bundle(
	struct bundle *const bundle,
	uint64_t fragment_offset, uint64_t fragment_length)
{
	// Invalid parameters
	if (fragment_offset + fragment_length > bundle->payload_block->length)
		return NULL;
	// No need for fragmentation
	else if (fragment_offset == 0 &&
		 fragment_length >= bundle->payload_block->length)
		return NULL;
	// Invalid: Empty fragment
	else if (fragment_length == 0)
		return NULL;

	// Create second fragment and initialize payload
	struct bundle *fragment = bundlefragmenter_create_new_fragment(
		bundle,
		true
	);

	if (fragment == NULL)
		return NULL;

	// Replicate required extension blocks
	const uint64_t total_adu_length = (
		bundle_is_fragmented(bundle)
		? bundle->total_adu_length
		: bundle->payload_block->length
	);
	const bool is_first = (bundle->fragment_offset + fragment_offset == 0);
	const bool is_last = (
		total_adu_length ==
		bundle->fragment_offset + fragment_offset + fragment_length
	);
	bool payload_found = false;
	struct bundle_block_list *cur_block = bundle->blocks;
	struct bundle_block_list **block_entry = &fragment->blocks;
	struct bundle_block_list *const payload_blist = fragment->blocks;
	struct bundle_block_list *entry = NULL;

	ASSERT(cur_block != NULL); // There has to be at least a payload block.
	ASSERT(fragment->blocks != NULL);
	if (bundle->blocks == NULL || fragment->blocks == NULL) {
		bundle_free(fragment);
		return NULL;
	}

	while (cur_block != NULL) {
		if (cur_block->data->type == BUNDLE_BLOCK_TYPE_PAYLOAD) {
			payload_found = true; // payload will not be copied here
			entry = payload_blist;
		} else if ((is_first && !payload_found) ||
			   (is_last && payload_found) ||
			   bundle_block_must_be_replicated(cur_block->data)) {
			entry = bundle_block_entry_dup(cur_block);

			// Something went south, free all replicated blocks
			if (entry == NULL) {
				*block_entry = payload_blist;
				bundle_free(fragment);
				return NULL;
			}
		} else {
			cur_block = cur_block->next;
			continue;
		}

		// Insert `entry` at the end
		ASSERT(entry != NULL);
		*block_entry = entry;
		block_entry = &entry->next;

		cur_block = cur_block->next;
	}
	ASSERT(payload_found);
	ASSERT(entry != NULL);
	if (entry == NULL || !payload_found) {
		bundle_free(fragment);
		return NULL;
	}
	// `entry` is always the last item that was added to the fragment.
	entry->data->flags |= BUNDLE_V6_BLOCK_FLAG_LAST_BLOCK;
	ASSERT(entry->next == NULL);

	// Copy fragment of payload block to pre-created payload block
	fragment->payload_block->length = fragment_length;
	fragment->payload_block->data = malloc(fragment->payload_block->length);
	if (fragment->payload_block->data == NULL) {
		bundle_free(fragment);
		return NULL;
	}
	memcpy(
		fragment->payload_block->data,
		bundle->payload_block->data + fragment_offset,
		fragment->payload_block->length
	);

	if (bundle_is_fragmented(bundle)) {
		fragment->fragment_offset = (
			bundle->fragment_offset + fragment_offset
		);
	} else {
		fragment->fragment_offset = fragment_offset;
	}

	// Recalculate primary block sizes
	if (bundle_recalculate_header_length(fragment) == UD3TN_FAIL) {
		bundle_free(fragment);
		return NULL;
	}

	return fragment;
}
