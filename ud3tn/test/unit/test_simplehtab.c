// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "ud3tn/simplehtab.h"

#include "testud3tn_unity.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

TEST_GROUP(simplehtab);

struct htab *htab;

TEST_SETUP(simplehtab)
{
	htab = htab_alloc(16);
}

TEST_TEAR_DOWN(simplehtab)
{
	htab_free(htab);
}

TEST(simplehtab, htab_alloc)
{
	struct htab *htab2;

	htab2 = htab_alloc(128);
	TEST_ASSERT_NULL(htab_get(htab2, "test"));
	htab_free(htab2);
}

TEST(simplehtab, htab_add)
{
	void *element = malloc(64), *get;
	struct htab_entrylist *r;

	TEST_ASSERT_NOT_NULL(element);
	r = htab_add(htab, "test", element);
	TEST_ASSERT_NOT_NULL(r);
	r = htab_add(htab, "test", element);
	TEST_ASSERT_NULL(r);
	get = htab_get(htab, "test");
	TEST_ASSERT(get == element);
	get = htab_remove(htab, "test");
	TEST_ASSERT(get == element);
	get = htab_remove(htab, "test");
	TEST_ASSERT(get == NULL);
	free(element);
}

TEST(simplehtab, htab_trunc)
{
	void *element = malloc(64), *get;
	struct htab_entrylist *r;

	TEST_ASSERT_NOT_NULL(element);
	r = htab_add(htab, "test", element);
	TEST_ASSERT_NOT_NULL(r);
	htab_trunc(htab);
	r = htab_add(htab, "test", element);
	TEST_ASSERT_NOT_NULL(r);
	get = htab_get(htab, "test");
	TEST_ASSERT(get == element);
	htab_trunc(htab);
	get = htab_get(htab, "test");
	TEST_ASSERT(get == NULL);
	free(element);
}

TEST(simplehtab, htab_add_many)
{
	const int count = 32;
	void *element = malloc(4);
	char *keys[count];
	uint16_t i;

	TEST_ASSERT_NOT_NULL(element);
	for (i = 0; i < count; i++) {
		keys[i] = malloc(5);
		snprintf(keys[i], 5, "%d", i);
		htab_add(htab, keys[i], element);
	}
	for (i = 0; i < count; i++)
		TEST_ASSERT_EQUAL_PTR(element, htab_get(htab, keys[i]));
	for (i = 0; i < count; i++) {
		htab_remove(htab, keys[i]);
		free(keys[i]);
	}
	free(element);
}

TEST(simplehtab, htab_update)
{
	const char *key = "test";
	struct htab_entrylist *r;
	void *element = malloc(64), *element2 = malloc(64);
	void *get = element; // initialize with non-NULL pointer

	TEST_ASSERT_NOT_NULL(element);
	TEST_ASSERT_NOT_NULL(element2);
	r = htab_update(htab, key, element, &get);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_PTR(element, r->value);
	TEST_ASSERT_NULL(get);
	get = htab_get(htab, key);
	TEST_ASSERT_EQUAL_PTR(element, get);
	r = htab_update(htab, key, element, &get);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_PTR(element, r->value);
	TEST_ASSERT_EQUAL_PTR(element, get);
	r = htab_update(htab, key, element2, &get);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_PTR(element2, r->value);
	TEST_ASSERT_EQUAL_PTR(element, get);
	get = htab_get(htab, key);
	TEST_ASSERT_EQUAL_PTR(element2, get);
	r = htab_update(htab, key, element2, &get);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_PTR(element2, r->value);
	TEST_ASSERT_EQUAL_PTR(element2, get);
	get = htab_remove(htab, key);
	TEST_ASSERT_EQUAL_PTR(element2, get);
	r = htab_update(htab, key, element2, &get);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_PTR(element2, r->value);
	TEST_ASSERT_NULL(get);
	get = htab_remove(htab, key);
	TEST_ASSERT_EQUAL_PTR(element2, get);
	free(element);
	free(element2);
}

TEST(simplehtab, htab_add_update_remove_known)
{
	const char *key = "test";
	void *element = malloc(64), *element2 = malloc(64), *get;
	struct htab_entrylist *r, *get_pair;
	const uint16_t hash = 55;

	TEST_ASSERT_NOT_NULL(element);
	TEST_ASSERT_NOT_NULL(element2);
	r = htab_add_known(htab, key, hash, strlen(key), element, 0);
	TEST_ASSERT_NOT_NULL(r);
	r = htab_add_known(htab, key, hash, strlen(key), element, 0);
	TEST_ASSERT_NULL(r);
	r = htab_update_known(htab, key, hash, strlen(key), element, &get, 0);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_PTR(element, get);
	get = htab_get(htab, key); // assumes that HASH(key) % slot_count != 55
	TEST_ASSERT_NULL(get);
	get = htab_get_known(htab, key, hash, 0);
	TEST_ASSERT_EQUAL_PTR(element, get);
	get_pair = htab_get_known_pair(htab, key, hash, 0);
	TEST_ASSERT_EQUAL_STRING(key, get_pair->key);
	TEST_ASSERT_EQUAL_PTR(element, get_pair->value);
	TEST_ASSERT_NULL(get_pair->next);
	// Test "compare pointer only" with other address for key
	// Note that key is copied upon insertion so the address changes!
	r = htab_add_known(htab, get_pair->key, hash, strlen(key), element, 1);
	TEST_ASSERT_NULL(r);
	r = htab_add_known(htab, key, hash, strlen(key), element, 1);
	TEST_ASSERT_NOT_NULL(r);
	get_pair = htab_get_known_pair(htab, key, hash, 0); // first
	TEST_ASSERT_EQUAL_STRING(key, get_pair->key);
	TEST_ASSERT_EQUAL_PTR(element, get_pair->value);
	TEST_ASSERT_NOT_NULL(get_pair->next);
	TEST_ASSERT_EQUAL_STRING(key, get_pair->next->key);
	TEST_ASSERT_EQUAL_PTR(element, get_pair->next->value);
	TEST_ASSERT_NULL(get_pair->next->next);
	get_pair = htab_get_known_pair(htab, get_pair->next->key, hash, 1);
	TEST_ASSERT_EQUAL_STRING(key, get_pair->key);
	TEST_ASSERT_EQUAL_PTR(element, get_pair->value);
	TEST_ASSERT_NULL(get_pair->next);
	r = htab_update_known(htab, get_pair->key, hash, strlen(key),
				element2, &get, 1);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_PTR(element, get);
	get_pair = htab_get_known_pair(htab, key, hash, 0); // first again
	TEST_ASSERT_EQUAL_PTR(element, get_pair->value);
	TEST_ASSERT_EQUAL_PTR(element2, get_pair->next->value);
	TEST_ASSERT_NULL(get_pair->next->next);
	// Test removal of everything
	get = htab_remove_known(htab, key, hash, 0); // rm 1st in list
	TEST_ASSERT_EQUAL_PTR(element, get);
	get = htab_remove_known(htab, key, hash, 0); // rm 2nd in list
	TEST_ASSERT_EQUAL_PTR(element2, get);
	get = htab_remove_known(htab, key, hash, 0);
	TEST_ASSERT_NULL(get);
	free(element);
	free(element2);
}

TEST_GROUP_RUNNER(simplehtab)
{
	RUN_TEST_CASE(simplehtab, htab_alloc);
	RUN_TEST_CASE(simplehtab, htab_add);
	RUN_TEST_CASE(simplehtab, htab_trunc);
	RUN_TEST_CASE(simplehtab, htab_add_many);
	RUN_TEST_CASE(simplehtab, htab_update);
	RUN_TEST_CASE(simplehtab, htab_add_update_remove_known);
}
