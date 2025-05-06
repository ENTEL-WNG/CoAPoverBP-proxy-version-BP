// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "ud3tn/agent_manager.h"
#include "ud3tn/bundle.h"
#include "ud3tn/common.h"
#include "ud3tn/fib.h"

#include "testud3tn_unity.h"

#include <stdlib.h>

static const char *ADM_SECRET = "this_is_a_very_secure_adm_secret";
static const agent_no_t FIRST_AGENT_NO = 1;
static const char *VALID_DEMUX_CHARS = (
	"!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
);
static const char *VALID_DEMUX_CHARS_PLUS_SPACE = (
	"!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~ "
);

static struct agent_manager *am, *am2;
static void *test_ptr1, *test_ptr2, *test_ptr3, *test_ptr4, *test_ptr5;

TEST_GROUP(agent_manager);

TEST_SETUP(agent_manager)
{
	am = agent_manager_create("dtn://ud3tn.dtn/", ADM_SECRET);
	am2 = agent_manager_create("ipn:1.0", ADM_SECRET);
	test_ptr1 = malloc(1);
	test_ptr2 = malloc(1);
	test_ptr3 = malloc(1);
	test_ptr4 = malloc(1);
	test_ptr5 = malloc(1);
}

TEST_TEAR_DOWN(agent_manager)
{
	agent_manager_free(am);
	agent_manager_free(am2);
	free(test_ptr1);
	free(test_ptr2);
	free(test_ptr3);
	free(test_ptr4);
	free(test_ptr5);
}

TEST(agent_manager, create_free)
{
	struct agent_manager *am_test;

	TEST_ASSERT_NOT_NULL(am);
	TEST_ASSERT_NOT_NULL(am2);
	TEST_ASSERT_NULL(agent_manager_create(NULL, ADM_SECRET));
	TEST_ASSERT_NULL(agent_manager_create("", ADM_SECRET));
	TEST_ASSERT_NULL(agent_manager_create("dtn", ADM_SECRET));
	TEST_ASSERT_NULL(agent_manager_create("https://ud3tn.dtn", ADM_SECRET));
	am_test = agent_manager_create("ipn:1.0", ADM_SECRET);
	TEST_ASSERT_NOT_NULL(am_test);
	agent_manager_free(am_test);
	am_test = agent_manager_create("ipn:1.0", NULL);
	if (IS_DEBUG_BUILD) {
		TEST_ASSERT_NOT_NULL(am_test);
		agent_manager_free(am_test);
	} else {
		TEST_ASSERT_NULL(am_test);
	}
	am_test = agent_manager_create("ipn:1.0", NULL);
	if (IS_DEBUG_BUILD) {
		TEST_ASSERT_NOT_NULL(am_test);
		agent_manager_free(am_test);
	} else {
		TEST_ASSERT_NULL(am_test);
	}
	TEST_ASSERT_GREATER_OR_EQUAL(1, AAP2_MINIMUM_PRE_SHARED_SECRET_LENGTH);
	am_test = agent_manager_create("ipn:1.0", "a");
	if (IS_DEBUG_BUILD) {
		TEST_ASSERT_NOT_NULL(am_test);
		agent_manager_free(am_test);
	} else {
		TEST_ASSERT_NULL(am_test);
	}
	agent_manager_free(NULL);
}

static void am_test_trx(struct bundle_adu data,
			void *param, const void *bp_context)
{
	TEST_ASSERT_EQUAL_PTR(test_ptr1, param);
	TEST_ASSERT_EQUAL_PTR(test_ptr2, bp_context);
	TEST_ASSERT_EQUAL_PTR(test_ptr3, data.payload);
}

static void am_test_fib(const struct fib_entry *fib_entry, const char *node_id,
			enum fib_link_status link_status,
			void *param, const void *bp_context)
{
	TEST_ASSERT_EQUAL_PTR(test_ptr1, param);
	TEST_ASSERT_EQUAL_PTR(test_ptr2, bp_context);
	TEST_ASSERT_EQUAL_PTR(test_ptr3, fib_entry);
	TEST_ASSERT_EQUAL_PTR(test_ptr4, node_id);
	TEST_ASSERT_EQUAL(FIB_LINK_STATUS_UP, link_status);
}

static bool am_test_bdm(struct bundle *bundle,
			enum bundle_dispatch_reason reason,
			const char *node_id,
			const char *cla_addr,
			void *param, const void *bp_context)
{
	TEST_ASSERT_EQUAL_PTR(test_ptr1, param);
	TEST_ASSERT_EQUAL_PTR(test_ptr2, bp_context);
	TEST_ASSERT_EQUAL_PTR(test_ptr3, bundle);
	TEST_ASSERT_EQUAL_PTR(test_ptr4, node_id);
	TEST_ASSERT_EQUAL_PTR(test_ptr5, cla_addr);
	TEST_ASSERT_EQUAL(DISPATCH_REASON_TX_FAILED, reason);
	return true;
}

static bool am_test_alive_true(void *param)
{
	(void)param;
	return true;
}

static bool am_test_alive_false(void *param)
{
	(void)param;
	return false;
}

TEST(agent_manager, reg)
{
	struct agent testagent1 = {
		.auth_trx = true,
		.is_subscriber = false,
		.trx_callback = am_test_trx,
		.param = test_ptr1,
	}, testagent2;
	agent_no_t rv = AGENT_NO_INVALID, rv2 = AGENT_NO_INVALID;

	// The following is not strictly necessary but can prevent some issues
	// when forgetting to set the number.
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, testagent1.agent_no);
	// dtn tests
	rv = agent_register(am, testagent1); // register without sink
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.is_subscriber = true;
	rv = agent_register(am, testagent1); // register without sink
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.is_subscriber = false;
	testagent1.sink_identifier = VALID_DEMUX_CHARS_PLUS_SPACE;
	rv = agent_register(am, testagent1); // register invalid sink
	testagent1.sink_identifier = VALID_DEMUX_CHARS;
	rv = agent_register(am, testagent1); // register valid sink
	TEST_ASSERT_EQUAL(FIRST_AGENT_NO, rv);
	rv2 = agent_register(am, testagent1); // register again
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv2);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, agent_deregister(am, rv)); // twice
	// test liveliness check
	testagent1.is_alive_callback = am_test_alive_true;
	rv = agent_register(am, testagent1); // register valid sink
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	rv2 = agent_register(am, testagent1); // register again
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv2); // same as above
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
	testagent1.is_alive_callback = am_test_alive_false;
	rv = agent_register(am, testagent1); // register valid sink
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	rv2 = agent_register(am, testagent1); // register again
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv2); // auto-deregistered
	TEST_ASSERT_EQUAL(rv2, agent_deregister(am, rv2));
	testagent1.is_alive_callback = NULL;
	// ipn tests
	rv = agent_register(am2, testagent1); // invalid sink for ipn
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.sink_identifier = "1"; // valid
	rv = agent_register(am2, testagent1);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am2, rv));
	testagent1.sink_identifier = "0"; // admin. endpoint is also valid
	rv = agent_register(am2, testagent1);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am2, rv));
	// reg. secret tests
	rv = agent_register(am, testagent1); // register again after deregister
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	testagent2 = testagent1;
	testagent2.is_subscriber = true;
	rv2 = agent_register(am, testagent2); // register other direction
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv2); // fails: secret == NULL
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
	TEST_ASSERT_EQUAL(rv2, agent_deregister(am, rv2));
	testagent2.secret = "my-secret-123";
	rv2 = agent_register(am, testagent2); // register again w/ secret
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv2);
	rv = agent_register(am, testagent1); // w/o secret
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.secret = "my-secret-456";
	rv = agent_register(am, testagent1); // w/ wrong secret
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.secret = "my-secret-123";
	rv = agent_register(am, testagent1); // w/ correct secret
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
	testagent1.secret = ADM_SECRET;
	rv = agent_register(am, testagent1); // w/ admin secret
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
	TEST_ASSERT_EQUAL(rv2, agent_deregister(am, rv2));
	// test missing callback
	testagent1.is_subscriber = true;
	testagent1.trx_callback = NULL;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.is_subscriber = false;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
	// test FIB registration
	testagent1.is_subscriber = true;
	testagent1.auth_trx = false;
	testagent1.auth_fib = true;
	rv = agent_register(am, testagent1); // missing callback
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.fib_callback = am_test_fib;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
	// test BDM registration
	testagent1.auth_bdm = true;
	rv = agent_register(am, testagent1); // missing callback
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.bdm_callback = am_test_bdm;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	// 2nd sub. BDM should NOT work
	testagent2 = testagent1;
	rv2 = agent_register(am, testagent2);
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv2);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
	// other direction
	testagent1.bdm_callback = NULL;
	testagent1.is_subscriber = false;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	// 2nd RPC BDM should work
	testagent2 = testagent1;
	rv2 = agent_register(am, testagent2);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv2);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
	TEST_ASSERT_EQUAL(rv2, agent_deregister(am, rv2));
	// test wrong secret
	testagent1.trx_callback = am_test_trx;
	testagent1.bdm_callback = am_test_bdm;
	testagent1.secret = "my-secret-123";
	rv = agent_register(am, testagent1);
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.is_subscriber = true;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.auth_bdm = false;
	testagent1.is_subscriber = true;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.is_subscriber = false;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.auth_fib = false;
	testagent1.is_subscriber = true;
	rv = agent_register(am, testagent1); // no auth specified
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.is_subscriber = false;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv);
	testagent1.auth_trx = true;
	testagent1.is_subscriber = true;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
	testagent1.is_subscriber = false;
	rv = agent_register(am, testagent1);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	TEST_ASSERT_EQUAL(rv, agent_deregister(am, rv));
}

TEST(agent_manager, fwd_to_sub)
{
	struct agent testagent1 = {
		.auth_trx = true,
		.is_subscriber = false,
		.sink_identifier = VALID_DEMUX_CHARS,
		.param = test_ptr1,
		.secret = ADM_SECRET,
	}, testagent2;
	agent_no_t rv, rv2, rv3;
	int rvi;
	struct bundle_adu data_bdl = {
		.payload = malloc(1),
	};

	rv = agent_register(am, testagent1);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv);
	rv2 = agent_forward(am, VALID_DEMUX_CHARS, data_bdl, test_ptr2);
	TEST_ASSERT_EQUAL(AGENT_NO_INVALID, rv2);
	data_bdl.payload = test_ptr3; // was freed by agent_forward!
	testagent2 = testagent1;
	testagent2.is_subscriber = true;
	testagent2.trx_callback = am_test_trx;
	rv3 = agent_register(am, testagent2);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv3);
	// TRX fwd test
	rv2 = agent_forward(am, VALID_DEMUX_CHARS, data_bdl, test_ptr2);
	TEST_ASSERT_EQUAL(rv3, rv2);
	// FIB fwd test
	rvi = agent_send_fib_info(
		am,
		test_ptr3,
		test_ptr4,
		FIB_LINK_STATUS_UP,
		test_ptr2
	);
	TEST_ASSERT_EQUAL(-1, rvi); // unregistered as FIB agent
	testagent2.is_subscriber = false;
	testagent2.auth_trx = false; // same sink -- if not TRX we can re-reg.
	testagent2.auth_fib = true;
	testagent2.fib_callback = am_test_fib;
	rv2 = agent_register(am, testagent2);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv2);
	rvi = agent_send_fib_info(
		am,
		test_ptr3,
		test_ptr4,
		FIB_LINK_STATUS_UP,
		test_ptr2
	);
	TEST_ASSERT_EQUAL(-1, rvi); // no subscriber
	TEST_ASSERT_EQUAL(rv2, agent_deregister(am, rv2));
	testagent2.is_subscriber = true;
	rv2 = agent_register(am, testagent2);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv2);
	rvi = agent_send_fib_info(
		am,
		test_ptr3,
		test_ptr4,
		FIB_LINK_STATUS_UP,
		test_ptr2
	);
	TEST_ASSERT_EQUAL(0, rvi);
	rvi = agent_handover_to_dispatch(
		am,
		test_ptr3,
		DISPATCH_REASON_TX_FAILED,
		test_ptr4,
		test_ptr5,
		test_ptr2
	);
	TEST_ASSERT_EQUAL(-1, rvi); // unregistered as BDM agent
	TEST_ASSERT_EQUAL(rv2, agent_deregister(am, rv2));
	testagent2.auth_bdm = true;
	testagent2.bdm_callback = am_test_bdm;
	rv2 = agent_register(am, testagent2);
	TEST_ASSERT_NOT_EQUAL(AGENT_NO_INVALID, rv2);
	rvi = agent_handover_to_dispatch(
		am,
		test_ptr3,
		DISPATCH_REASON_TX_FAILED,
		test_ptr4,
		test_ptr5,
		test_ptr2
	);
	TEST_ASSERT_EQUAL(0, rvi);
}

TEST_GROUP_RUNNER(agent_manager)
{
	RUN_TEST_CASE(agent_manager, create_free);
	RUN_TEST_CASE(agent_manager, reg);
	RUN_TEST_CASE(agent_manager, fwd_to_sub);
}
