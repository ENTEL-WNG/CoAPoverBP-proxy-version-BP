// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef BUNDLE7_FRAGMENT_H_INCLUDED
#define BUNDLE7_FRAGMENT_H_INCLUDED

#include "ud3tn/bundle.h"

#include <stdint.h>

struct bundle *bundle7_fragment_bundle(
	struct bundle *bundle,
	uint64_t fragment_offset, uint64_t fragment_length);

#endif // BUNDLE7_FRAGMENT_H_INCLUDED
