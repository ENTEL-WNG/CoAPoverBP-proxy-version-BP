// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "cla/cla.h"
#include "cla/cla_contact_tx_task.h"

#include "bundle6/parser.h"
#include "bundle7/parser.h"

#include "platform/hal_io.h"
#include "platform/hal_semaphore.h"
#include "platform/hal_task.h"
#include "platform/hal_time.h"

#include "ud3tn/bundle.h"
#include "ud3tn/bundle_processor.h"
#include "ud3tn/common.h"
#include "ud3tn/result.h"

#include <stdbool.h>
#include <stdlib.h>

#if defined(CLA_TX_RATE_LIMIT) && CLA_TX_RATE_LIMIT != 0
static const int rate_sleep_time_ms = 1000 / CLA_TX_RATE_LIMIT;
#else // CLA_TX_RATE_LIMIT
static const int rate_sleep_time_ms;
#endif // CLA_TX_RATE_LIMIT

// BPv7 5.4-4 / RFC5050 5.4-5
static void prepare_bundle_for_forwarding(struct bundle *bundle)
{
	struct bundle_block_list **blocks = &bundle->blocks;

	// BPv7 5.4-4: "If the bundle has a Previous Node block ..., then that
	// block MUST be removed ... before the bundle is forwarded."
	while (*blocks != NULL) {
		if ((*blocks)->data->type == BUNDLE_BLOCK_TYPE_PREVIOUS_NODE) {
			// Replace the first occurrence of the previous node
			// block by its successor and free it.
			*blocks = bundle_block_entry_free(*blocks);
			break;
		}
		blocks = &(*blocks)->next;
	}

	const uint64_t dwell_time_ms = hal_time_get_timestamp_ms() -
		bundle->reception_timestamp_ms;

	// BPv7 5.4-4: "If the bundle has a bundle age block ... at the last
	// possible moment ... the bundle age value MUST be increased ..."
	if (bundle_age_update(bundle, dwell_time_ms) == UD3TN_FAIL)
		LOGF_ERROR("TX: Bundle %p age block update failed!", bundle);
}

static void bp_inform_tx(QueueIdentifier_t signaling_queue,
			 struct bundle *const b,
			 struct cla_link *const link,
			 char *node_id,
			 char *cla_addr,
			 const bool success)
{
	bundle_processor_inform(
		signaling_queue,
		(struct bundle_processor_signal) {
			.type = (
				success
				? BP_SIGNAL_TRANSMISSION_SUCCESS
				: BP_SIGNAL_TRANSMISSION_FAILURE
			),
			.bundle = b,
			// NOTE: Ownership for both is transferred to BP!
			.peer_node_id = node_id,
			.peer_cla_addr = cla_addr,
		}
	);
}

struct cla_param {
	struct cla_link *link;
	enum ud3tn_result (*cla_send_fun)(struct cla_link *, const void *, size_t);
};

static enum ud3tn_result send_packet_data_callback(
	void *cla_param, const void *data, const size_t length)
{
	struct cla_param *p = cla_param;

	return p->cla_send_fun(p->link, data, length);
}

static void cla_contact_tx_task(void *param)
{
	struct cla_link *link = param;
	struct cla_contact_tx_task_command cmd;

	enum ud3tn_result s = UD3TN_FAIL;
	enum cla_begin_packet_result begin_packet_result;
	QueueIdentifier_t signaling_queue =
		link->config->bundle_agent_interface->bundle_signaling_queue;

	for (;;) {
		if (hal_queue_receive(link->tx_queue_handle,
				      &cmd, -1) == UD3TN_FAIL)
			continue;
		else if (cmd.type == TX_COMMAND_FINALIZE || !cmd.bundle)
			break;

		struct bundle *const b = cmd.bundle;
		const struct cla_vtable *const vtable = link->config->vtable;

		prepare_bundle_for_forwarding(b);
		LOGF_DEBUG(
			"TX: Sending bundle %p via CLA %s",
			b,
			vtable->cla_name_get()
		);
		begin_packet_result = vtable->cla_begin_packet(
			link,
			b,
			bundle_get_serialized_size(b),
			cmd.cla_address
		);

		s = UD3TN_FAIL;

		if (begin_packet_result == CLA_BEGIN_PACKET_OK) {
			struct cla_param send_param = {
				.link = link,
				.cla_send_fun = vtable->cla_send_packet_data,
			};

			s = bundle_serialize(
				b,
				send_packet_data_callback,
				&send_param
			);
		}

		if (s == UD3TN_OK)
			s = vtable->cla_end_packet(link);

		// NOTE that we own both cmd.node_id and cmd.cla_address, as the
		// BP copies it before sending the signal, because the original
		// copy may be deleted in the meantime. We hand the ownership
		// back to the BP.
		if (begin_packet_result == CLA_BEGIN_PACKET_DONE || s == UD3TN_OK) {
			bp_inform_tx(
				signaling_queue,
				b,
				link,
				cmd.node_id,
				cmd.cla_address,
				true
			);
		} else {
			bp_inform_tx(
				signaling_queue,
				b,
				link,
				cmd.node_id,
				cmd.cla_address,
				false
			);
		}


		if (rate_sleep_time_ms)
			hal_task_delay(rate_sleep_time_ms);
	}

	// Lock the queue before we start to free it
	hal_semaphore_take_blocking(link->tx_queue_sem);

	// Consume the rest of the queue
	while (hal_queue_receive(link->tx_queue_handle, &cmd, 0) != UD3TN_FAIL) {
		if (cmd.type == TX_COMMAND_BUNDLE) {
			bp_inform_tx(
				signaling_queue,
				cmd.bundle,
				link,
				cmd.node_id,
				cmd.cla_address,
				false
			);
		}
	}

	hal_semaphore_release(link->tx_task_sem);
}

enum ud3tn_result cla_launch_contact_tx_task(struct cla_link *link)
{
	hal_semaphore_take_blocking(link->tx_task_sem);

	const enum ud3tn_result res = hal_task_create(
		cla_contact_tx_task,
		link,
		true,
		NULL
	);

	// Not launched, no need to wait for exit.
	if (res != UD3TN_OK)
		hal_semaphore_release(link->tx_task_sem);

	return res;
}

void cla_contact_tx_task_request_exit(QueueIdentifier_t queue)
{
	struct cla_contact_tx_task_command command = {
		.type = TX_COMMAND_FINALIZE,
		.bundle = NULL,
		.cla_address = NULL,
	};

	ASSERT(queue != NULL);
	hal_queue_push_to_back(queue, &command);
}
