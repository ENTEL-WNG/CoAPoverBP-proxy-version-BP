// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "bundle6/create.h"
#include "bundle6/fragment.h"

#include "ud3tn/bundle.h"

#include "platform/hal_time.h"

#include "testud3tn_unity.h"

#include <string.h>
#include <stdlib.h>

static const char test_payload[] = "PAYLOAD1PAYLOAD2";
static struct bundle *b;

TEST_GROUP(bundle6Fragment);

TEST_SETUP(bundle6Fragment)
{
	char *payload = malloc(sizeof(test_payload));
	uint64_t creation_timestamp_s = 658489863; // 2020-11-12T09:51:03+00:00

	memcpy(payload, test_payload, sizeof(test_payload));
	b = bundle6_create_local(
		payload,
		sizeof(test_payload),
		"dtn:sourceeid",
		"dtn:desteid",
		creation_timestamp_s * 1000, 1, 42 * 1000,
		BUNDLE_FLAG_REPORT_DELIVERY |
		BUNDLE_V6_FLAG_CUSTODY_TRANSFER_REQUESTED
	);
	b->report_to = strdup("dtn:reportto");
	b->current_custodian = strdup("dtn:custodian");

	struct bundle_block *block_before = bundle_block_create(42);

	block_before->length = 1;
	block_before->data = malloc(1);
	block_before->data[0] = 0x42;
	block_before->flags = BUNDLE_BLOCK_FLAG_NONE;

	struct bundle_block *block_repl = bundle_block_create(43);

	block_repl->length = 1;
	block_repl->data = malloc(1);
	block_repl->data[0] = 0x42;
	block_repl->flags = BUNDLE_BLOCK_FLAG_MUST_BE_REPLICATED;

	struct bundle_block *block_after = bundle_block_create(44);

	block_after->length = 1;
	block_after->data = malloc(1);
	block_after->data[0] = 0x42;
	block_after->flags = BUNDLE_V6_BLOCK_FLAG_LAST_BLOCK;

	struct bundle_block_list *bl_before = bundle_block_entry_create(block_before);
	struct bundle_block_list *bl_repl = bundle_block_entry_create(block_repl);
	struct bundle_block_list *bl_after = bundle_block_entry_create(block_after);

	bl_before->next = bl_repl;
	bl_repl->next = b->blocks;
	b->blocks->next = bl_after;
	b->blocks = bl_before;
	b->payload_block->flags = BUNDLE_BLOCK_FLAG_NONE;
}

TEST_TEAR_DOWN(bundle6Fragment)
{
	bundle_free(b);
}

TEST(bundle6Fragment, fragment_bundle)
{
	const size_t pl_len = sizeof(test_payload);

	TEST_ASSERT_NULL(bundle6_fragment_bundle(b, pl_len, 1));
	TEST_ASSERT_NULL(bundle6_fragment_bundle(b, pl_len, 0));
	TEST_ASSERT_NULL(bundle6_fragment_bundle(b, 0, 0));
	TEST_ASSERT_NULL(bundle6_fragment_bundle(b, 0, pl_len));
	TEST_ASSERT_NULL(bundle6_fragment_bundle(b, 1, pl_len));

	struct bundle *frag1 = bundle6_fragment_bundle(b, 0, 8);

	TEST_ASSERT_NOT_NULL(frag1);

	struct bundle *frag2 = bundle6_fragment_bundle(b, 8, pl_len - 8);

	TEST_ASSERT_NOT_NULL(frag2);

	struct bundle *frag3 = bundle6_fragment_bundle(frag2, 3, 4);

	TEST_ASSERT_NOT_NULL(frag3);

	TEST_ASSERT_EQUAL(sizeof("PAYLOAD1") - 1, frag1->payload_block->length);
	TEST_ASSERT_EQUAL_UINT8_ARRAY(
		(const uint8_t *)"PAYLOAD1",
		frag1->payload_block->data,
		frag1->payload_block->length
	);
	TEST_ASSERT_EQUAL(sizeof("PAYLOAD2"), frag2->payload_block->length);
	TEST_ASSERT_EQUAL_UINT8_ARRAY(
		(const uint8_t *)"PAYLOAD2",
		frag2->payload_block->data,
		frag2->payload_block->length
	);
	TEST_ASSERT_EQUAL(sizeof("LOAD") - 1, frag3->payload_block->length);
	TEST_ASSERT_EQUAL_UINT8_ARRAY(
		(const uint8_t *)"LOAD",
		frag3->payload_block->data,
		frag3->payload_block->length
	);

	TEST_ASSERT_NOT_NULL(frag1->blocks); // block_before
	TEST_ASSERT_NOT_NULL(frag1->blocks->data);
	TEST_ASSERT_EQUAL(42, frag1->blocks->data->type);
	TEST_ASSERT_NOT_NULL(frag1->blocks->next); // block_repl
	TEST_ASSERT_NOT_NULL(frag1->blocks->next->data);
	TEST_ASSERT_EQUAL(43, frag1->blocks->next->data->type);
	TEST_ASSERT_NOT_NULL(frag1->blocks->next->next); // payload
	TEST_ASSERT_EQUAL_PTR(frag1->blocks->next->next->data,
			      frag1->payload_block);
	TEST_ASSERT_NULL(frag1->blocks->next->next->next);

	TEST_ASSERT_NOT_NULL(frag2->blocks); // block_repl
	TEST_ASSERT_NOT_NULL(frag2->blocks->data);
	TEST_ASSERT_EQUAL(43, frag2->blocks->data->type);
	TEST_ASSERT_NOT_NULL(frag2->blocks->next); // payload
	TEST_ASSERT_EQUAL_PTR(frag2->blocks->next->data, frag2->payload_block);
	TEST_ASSERT_NOT_NULL(frag2->blocks->next->next); // block_after
	TEST_ASSERT_NOT_NULL(frag2->blocks->next->next->data);
	TEST_ASSERT_EQUAL(44, frag2->blocks->next->next->data->type);
	TEST_ASSERT_NULL(frag2->blocks->next->next->next);

	TEST_ASSERT_NOT_NULL(frag3->blocks); // block_repl
	TEST_ASSERT_NOT_NULL(frag3->blocks->data);
	TEST_ASSERT_EQUAL(43, frag3->blocks->data->type);
	TEST_ASSERT_NOT_NULL(frag3->blocks->next); // payload
	TEST_ASSERT_EQUAL_PTR(frag3->blocks->next->data, frag3->payload_block);
	TEST_ASSERT_NULL(frag3->blocks->next->next);

	bundle_free(frag1);
	bundle_free(frag2);
	bundle_free(frag3);
}

TEST_GROUP_RUNNER(bundle6Fragment)
{
	RUN_TEST_CASE(bundle6Fragment, fragment_bundle);
}

