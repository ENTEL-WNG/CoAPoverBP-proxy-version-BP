// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef BUNDLEFRAGMENTER_H_INCLUDED
#define BUNDLEFRAGMENTER_H_INCLUDED

#include "ud3tn/bundle.h"

#include <stdbool.h>
#include <stdint.h>


/**
 * Creates a new fragment of the given bundle.
 *
 * @param bundle The bundle of which a fragment shall be created.
 * @param fragment_offset The payload offset, relative to the provided bundle
 *                        payload block (even if it is already a fragment),
 *                        in bytes.
 * @param fragment_length The fragment payload length in bytes.
 *
 * @return a new bundle, containing a copy of the specified part of the payload
 *         plus extension blocks as defined by the Bundle Protocol, or NULL
 *         in case of error.
 */
struct bundle *bundlefragmenter_fragment_bundle(
	struct bundle *bundle,
	uint64_t fragment_offset, uint64_t fragment_length);


/**
 * Creates a new fragment for the given bundle
 *
 * @param init_payload If true, any existing payload block will be freed and a
 *                     new payload block with size zero will be created
 *
 * @return in case if any error NULL will be returned, otherwise the newly
 *         created fragment
 */
struct bundle *bundlefragmenter_create_new_fragment(
	struct bundle const *prototype, bool init_payload);


#endif /* BUNDLEFRAGMENTER_H_INCLUDED */
