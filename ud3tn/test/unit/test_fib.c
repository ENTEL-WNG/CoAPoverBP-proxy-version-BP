// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ud3tn/fib.h"

#include "testud3tn_unity.h"

#include <stdbool.h>
#include <string.h>

TEST_GROUP(fib);

static struct fib *fib;

static const char *TEST_CLA_ADDR_1 = "mtcp:test.example:1234";
static const char *TEST_CLA_ADDR_2 = "mtcp:test.example:5678";
static const char *TEST_CLA_ADDR_3 = "tcpclv3:127.0.0.1:1234";
static const char *TEST_CLA_ADDR_4 = "tcpspp:";

static const char *TEST_NODE_ID_1 = "ipn:1.0";
static const char *TEST_NODE_ID_2 = "ipn:2.0";
static const char *TEST_NODE_ID_3 = "dtn://node3.dtn/";
static const char *TEST_NODE_ID_4 = "myscheme:node4";

TEST_SETUP(fib)
{
	fib = fib_alloc();
}

TEST_TEAR_DOWN(fib)
{
	fib_free(fib);
}

TEST(fib, fib_insert_lookup_remove_link)
{
	struct fib_link *r, *r2;

	// Add a single element
	r = fib_insert_link(fib, TEST_CLA_ADDR_1);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_NOT_EQUAL(TEST_CLA_ADDR_1, r->cla_addr);
	TEST_ASSERT_EQUAL_STRING(TEST_CLA_ADDR_1, r->cla_addr);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_UNSPECIFIED, r->status);
	r->status = FIB_LINK_STATUS_UP;
	// Try to insert it twice
	r2 = fib_insert_link(fib, TEST_CLA_ADDR_1);
	TEST_ASSERT_EQUAL_PTR(r, r2);
	TEST_ASSERT_EQUAL_STRING(TEST_CLA_ADDR_1, r->cla_addr);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_UP, r->status);
	r2 = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_1);
	TEST_ASSERT_EQUAL_PTR(r, r2);
	// Add a second element
	r = fib_insert_link(fib, TEST_CLA_ADDR_2);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_NOT_EQUAL(TEST_CLA_ADDR_2, r->cla_addr);
	TEST_ASSERT_EQUAL_STRING(TEST_CLA_ADDR_2, r->cla_addr);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_UNSPECIFIED, r->status);
	TEST_ASSERT_NOT_EQUAL(r2, r);
	r2 = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_2);
	TEST_ASSERT_EQUAL_PTR(r, r2);
	r->status = FIB_LINK_STATUS_DOWN;
	r2 = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_2);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_DOWN, r2->status);
	// Remove something that does not exist
	TEST_ASSERT_FALSE(fib_remove_link(fib, TEST_CLA_ADDR_3));
	// Remove first element, check that second remains
	TEST_ASSERT_TRUE(fib_remove_link(fib, TEST_CLA_ADDR_1));
	r = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_1);
	TEST_ASSERT_NULL(r);
	r = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_2);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_STRING(TEST_CLA_ADDR_2, r->cla_addr);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_DOWN, r->status);
	// Remove rest, twice
	TEST_ASSERT_TRUE(fib_remove_link(fib, TEST_CLA_ADDR_2));
	r = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_2);
	TEST_ASSERT_NULL(r);
	TEST_ASSERT_FALSE(fib_remove_link(fib, TEST_CLA_ADDR_2));
	r = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_2);
	TEST_ASSERT_NULL(r);
	// Try re-insert
	r = fib_insert_link(fib, TEST_CLA_ADDR_2);
	TEST_ASSERT_NOT_NULL(r);
	r2 = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_2);
	TEST_ASSERT_EQUAL_PTR(r, r2);
}

TEST(fib, fib_insert_lookup_remove_node)
{
	struct fib_entry *r, *r2;
	struct fib_link *l, *l2;

	// Add a single element
	r = fib_insert_node(fib, TEST_NODE_ID_1, TEST_CLA_ADDR_1);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_NOT_EQUAL(TEST_CLA_ADDR_1, r->cla_addr);
	TEST_ASSERT_EQUAL_STRING(TEST_CLA_ADDR_1, r->cla_addr);
	TEST_ASSERT_EQUAL(FIB_ENTRY_FLAG_NONE, r->flags);
	l = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_1);
	TEST_ASSERT_NOT_NULL(l);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_UNSPECIFIED, l->status);
	l->status = FIB_LINK_STATUS_UP;
	// Try to insert it twice
	r2 = fib_insert_node(fib, TEST_NODE_ID_1, TEST_CLA_ADDR_1);
	TEST_ASSERT_NOT_NULL(r2);
	TEST_ASSERT_EQUAL_STRING(TEST_CLA_ADDR_1, r2->cla_addr);
	l2 = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_1);
	TEST_ASSERT_EQUAL_PTR(l, l2);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_UP, l2->status);
	r = fib_lookup_node(fib, TEST_NODE_ID_1);
	TEST_ASSERT_EQUAL_PTR(r2, r);
	// Change the CLA address
	r = fib_insert_node(fib, TEST_NODE_ID_1, TEST_CLA_ADDR_4);
	TEST_ASSERT_NOT_EQUAL(r2, r);
	TEST_ASSERT_EQUAL_STRING(TEST_CLA_ADDR_4, r->cla_addr);
	l = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_4);
	TEST_ASSERT_NOT_NULL(l);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_UNSPECIFIED, l->status);
	// Make link active so it does not get deleted with the node.
	l->status = FIB_LINK_STATUS_UP;
	// Check that Link and status is kept
	l = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_1);
	TEST_ASSERT_NOT_NULL(l);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_UP, l->status);
	// Add a second element, Link first
	l = fib_insert_link(fib, TEST_CLA_ADDR_2);
	TEST_ASSERT_NOT_NULL(l);
	TEST_ASSERT_NULL(fib_lookup_node(fib, TEST_NODE_ID_2));
	r2 = fib_insert_node(fib, TEST_NODE_ID_2, TEST_CLA_ADDR_2);
	TEST_ASSERT_NOT_NULL(r2);
	l2 = fib_lookup_cla_addr(fib, r2->cla_addr);
	TEST_ASSERT_EQUAL_PTR(l, l2);
	l->status = FIB_LINK_STATUS_DOWN;
	r2 = fib_lookup_node(fib, TEST_NODE_ID_2);
	TEST_ASSERT_NOT_NULL(r2);
	l2 = fib_lookup_cla_addr(fib, r2->cla_addr);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_DOWN, l2->status);
	// Remove something that does not exist
	TEST_ASSERT_FALSE(fib_remove_node(fib, TEST_NODE_ID_3, false));
	// Remove an existing and mapped Link -> fails
	TEST_ASSERT_FALSE(fib_remove_link(fib, TEST_CLA_ADDR_4));
	// Remove first element, check that second remains
	TEST_ASSERT_TRUE(fib_remove_node(fib, TEST_NODE_ID_1, false));
	r = fib_lookup_node(fib, TEST_NODE_ID_1);
	TEST_ASSERT_NULL(r);
	r = fib_lookup_node(fib, TEST_NODE_ID_2);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_STRING(TEST_CLA_ADDR_2, r->cla_addr);
	l = fib_lookup_cla_addr(fib, r->cla_addr);
	TEST_ASSERT_NOT_NULL(l);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_DOWN, l->status);
	// Check that Links are still there
	l = fib_lookup_cla_addr(fib, TEST_CLA_ADDR_1);
	TEST_ASSERT_NOT_NULL(l);
	TEST_ASSERT_NOT_NULL(fib_lookup_cla_addr(fib, TEST_CLA_ADDR_4));
	// Check that association is re-created
	r = fib_insert_node(fib, TEST_NODE_ID_1, TEST_CLA_ADDR_1);
	TEST_ASSERT_NOT_NULL(r);
	l2 = fib_lookup_cla_addr(fib, r->cla_addr);
	TEST_ASSERT_EQUAL_PTR(l, l2);
	// Check removal of all mappings
	fib_insert_node(fib, TEST_NODE_ID_1, TEST_CLA_ADDR_2);
	// This should delete also TEST_NODE_ID_2 (maps to CLA addr. 2)
	TEST_ASSERT_TRUE(fib_remove_node(fib, TEST_NODE_ID_1, true));
	TEST_ASSERT_NULL(fib_lookup_node(fib, TEST_NODE_ID_1));
	TEST_ASSERT_NULL(fib_lookup_node(fib, TEST_NODE_ID_2));
	// Remove a 2nd time -> should fail
	TEST_ASSERT_FALSE(fib_remove_node(fib, TEST_NODE_ID_2, false));
	TEST_ASSERT_FALSE(fib_remove_node(fib, TEST_NODE_ID_2, true));
	TEST_ASSERT_NULL(fib_lookup_node(fib, TEST_NODE_ID_2));
	TEST_ASSERT_FALSE(fib_remove_node(fib, TEST_NODE_ID_1, false));
	TEST_ASSERT_FALSE(fib_remove_node(fib, TEST_NODE_ID_1, true));
	TEST_ASSERT_NULL(fib_lookup_node(fib, TEST_NODE_ID_1));
}

#define FIB_FOREACH_TEST_COUNT 4

struct fib_foreach_test_ctx {
	const char **expected_node_ids;
	struct fib_entry **expected_entries;
	struct fib_link **expected_links;
	bool *evaluated;
	int i, max, tested_count;
};

bool fib_foreach_test_fun(void *context, const char *node_id,
			  const struct fib_entry *entry,
			  const struct fib_link *link)
{
	struct fib_foreach_test_ctx *ctx = context;

	for (int i = 0; i < ctx->tested_count; i++) {
		if (!strcmp(ctx->expected_node_ids[i], node_id)) {
			TEST_ASSERT_EQUAL_PTR(
				ctx->expected_entries[i],
				entry
			);
			TEST_ASSERT_EQUAL_PTR(
				ctx->expected_links[i],
				link
			);
			ctx->evaluated[i] = true;
			ctx->i++;
			break;
		}
	}

	if (ctx->i >= ctx->max)
		return false;

	return true;
}

TEST(fib, fib_foreach)
{
	const char *nids[FIB_FOREACH_TEST_COUNT] = {
		TEST_NODE_ID_1,
		TEST_NODE_ID_2,
		TEST_NODE_ID_3,
		TEST_NODE_ID_4,
	};
	struct fib_entry *e[FIB_FOREACH_TEST_COUNT];
	struct fib_link *el[FIB_FOREACH_TEST_COUNT];
	bool was_evaluated[FIB_FOREACH_TEST_COUNT] = {};

	e[0] = fib_insert_node(fib, nids[0], TEST_CLA_ADDR_1);
	TEST_ASSERT_NOT_NULL(e[0]);
	el[0] = fib_lookup_cla_addr(fib, e[0]->cla_addr);
	TEST_ASSERT_NOT_NULL(el[0]);
	e[1] = fib_insert_node(fib, nids[1], TEST_CLA_ADDR_1);
	TEST_ASSERT_NOT_NULL(e[1]);
	el[1] = fib_lookup_cla_addr(fib, e[1]->cla_addr);
	TEST_ASSERT_NOT_NULL(el[1]);
	TEST_ASSERT_EQUAL_PTR(el[0], el[1]);
	el[2] = fib_insert_link(fib, TEST_CLA_ADDR_2);
	TEST_ASSERT_NOT_NULL(el[2]);
	e[2] = fib_insert_node(fib, nids[2], TEST_CLA_ADDR_2);
	TEST_ASSERT_NOT_NULL(e[2]);
	TEST_ASSERT_EQUAL_PTR(el[2], fib_lookup_cla_addr(fib, e[2]->cla_addr));
	e[3] = fib_insert_node(fib, nids[3], TEST_CLA_ADDR_3);
	TEST_ASSERT_NOT_NULL(e[3]);
	el[3] = fib_lookup_cla_addr(fib, e[3]->cla_addr);
	TEST_ASSERT_NOT_NULL(el[3]);

	const struct fib_foreach_test_ctx ctx_proto = {
		.expected_node_ids = nids,
		.expected_entries = e,
		.expected_links = el,
		.evaluated = was_evaluated,
		.i = 0,
		.max = FIB_FOREACH_TEST_COUNT,
		.tested_count = FIB_FOREACH_TEST_COUNT,
	};
	struct fib_foreach_test_ctx ctx = ctx_proto;
	int true_count;

	fib_foreach(fib, fib_foreach_test_fun, &ctx, NULL);
	for (int i = 0; i < FIB_FOREACH_TEST_COUNT; i++)
		TEST_ASSERT_TRUE(ctx.evaluated[i]);
	TEST_ASSERT_EQUAL_INT(FIB_FOREACH_TEST_COUNT, ctx.i);

	ctx = ctx_proto;
	ctx.max = 0;
	for (int i = 0; i < FIB_FOREACH_TEST_COUNT; i++)
		ctx.evaluated[i] = false;

	fib_foreach(fib, fib_foreach_test_fun, &ctx, NULL);
	true_count = 0;
	for (int i = 0; i < FIB_FOREACH_TEST_COUNT; i++)
		if (ctx.evaluated[i])
			true_count++;
	// Due to the definition of fib_foreach_test_fun and the nature of the
	// callback mechanism, the callback will always be called at least once
	// as long as there are elements inside the FIB.
	TEST_ASSERT_EQUAL_INT(1, true_count);
	TEST_ASSERT_EQUAL_INT(1, ctx.i);

	ctx = ctx_proto;
	ctx.max = 3;
	for (int i = 0; i < FIB_FOREACH_TEST_COUNT; i++)
		ctx.evaluated[i] = false;

	fib_foreach(fib, fib_foreach_test_fun, &ctx, NULL);
	true_count = 0;
	for (int i = 0; i < FIB_FOREACH_TEST_COUNT; i++)
		if (ctx.evaluated[i])
			true_count++;
	TEST_ASSERT_EQUAL_INT(3, true_count);
	TEST_ASSERT_EQUAL_INT(3, ctx.i);

	ctx = ctx_proto;
	for (int i = 0; i < FIB_FOREACH_TEST_COUNT; i++)
		ctx.evaluated[i] = false;

	fib_foreach(fib, fib_foreach_test_fun, &ctx, TEST_CLA_ADDR_1);
	for (int i = 0; i < 2; i++)
		TEST_ASSERT_TRUE(ctx.evaluated[i]);
	for (int i = 2; i < FIB_FOREACH_TEST_COUNT; i++)
		TEST_ASSERT_FALSE(ctx.evaluated[i]);

	ctx = ctx_proto;
	ctx.max = 1;
	for (int i = 0; i < FIB_FOREACH_TEST_COUNT; i++)
		ctx.evaluated[i] = false;

	fib_foreach(fib, fib_foreach_test_fun, &ctx, TEST_CLA_ADDR_1);
	true_count = 0;
	for (int i = 0; i < FIB_FOREACH_TEST_COUNT; i++)
		if (ctx.evaluated[i])
			true_count++;
	TEST_ASSERT_EQUAL_INT(1, true_count);
	TEST_ASSERT_EQUAL_INT(1, ctx.i);
}

TEST_GROUP_RUNNER(fib)
{
	RUN_TEST_CASE(fib, fib_insert_lookup_remove_link);
	RUN_TEST_CASE(fib, fib_insert_lookup_remove_node);
	RUN_TEST_CASE(fib, fib_foreach);
}
