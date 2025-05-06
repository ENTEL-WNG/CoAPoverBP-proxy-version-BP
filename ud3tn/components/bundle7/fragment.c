// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "bundle7/fragment.h"

#include "ud3tn/bundle.h"
#include "ud3tn/bundle_fragmenter.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// 5.8. Bundle Fragmentation
struct bundle *bundle7_fragment_bundle(
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

	// Create second fragment and initialize payload
	struct bundle *fragment = bundlefragmenter_create_new_fragment(
		bundle,
		false
	);

	if (fragment == NULL)
		return NULL;

	// Replicate required extension blocks
	const bool is_first = (bundle->fragment_offset + fragment_offset == 0);
	struct bundle_block_list *cur_block = bundle->blocks;
	struct bundle_block_list *prev = NULL;

	while (cur_block != NULL) {
		if ((is_first ||
		     bundle_block_must_be_replicated(cur_block->data)) &&
		    cur_block->data->type != BUNDLE_BLOCK_TYPE_PAYLOAD) {
			struct bundle_block_list *entry =
				bundle_block_entry_dup(cur_block);

			// Something went south, free all replicated blocks
			if (entry == NULL) {
				bundle_free(fragment);
				return NULL;
			}

			// Link previous block to duplicate
			if (prev != NULL)
				prev->next = entry;
			// Set first block entry
			else
				fragment->blocks = entry;

			prev = entry;
		}
		cur_block = cur_block->next;
	}

	// Copy fragment of payload block
	fragment->payload_block = bundle_block_create(
		BUNDLE_BLOCK_TYPE_PAYLOAD
	);
	if (!fragment->payload_block) {
		bundle_free(fragment);
		return NULL;
	}
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

	// Link last block with payload block
	struct bundle_block_list *payload_entry = bundle_block_entry_create(
		fragment->payload_block
	);

	if (payload_entry == NULL) {
		bundle_free(fragment);
		return NULL;
	}

	if (prev != NULL)
		prev->next = payload_entry;
	else
		fragment->blocks = payload_entry;

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
