// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "routing/compat/contact_manager.h"
#include "routing/compat/node.h"
#include "routing/compat/routing_table.h"

#include "ud3tn/common.h"

#include "cla/cla.h"
#include "cla/cla_contact_tx_task.h"

#include "platform/hal_io.h"
#include "platform/hal_platform.h"
#include "platform/hal_queue.h"
#include "platform/hal_semaphore.h"
#include "platform/hal_task.h"
#include "platform/hal_time.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define MAX_CONCURRENT_CONTACTS ROUTER_MAX_CONCURRENT_CONTACTS

struct contact_manager_task_parameters {
	Semaphore_t semaphore;
	QueueIdentifier_t control_queue;
	QueueIdentifier_t bp_queue;
	struct contact_list **contact_list_ptr;
};

struct contact_info {
	char *eid;
	char *cla_addr;
	uint64_t to_ms;
};

struct contact_info_list {
	struct contact_info contact_info;
	struct contact_info_list *next;
};

struct contact_manager_context {
	struct contact_info_list *current_contacts;
	int8_t current_contact_count;
	uint64_t next_contact_time_ms;
};

static bool contact_active(
	const struct contact_manager_context *const ctx,
	const struct contact *contact)
{
	for (struct contact_info_list *cur = ctx->current_contacts; cur != NULL; cur = cur->next) {
		const struct contact_info ci = cur->contact_info;

		if (!strcmp(ci.eid, contact->node->eid) && ci.to_ms == contact->to_ms)
			return true;
	}
	return false;
}

static int8_t remove_expired_contacts(
	struct contact_manager_context *const ctx,
	const uint64_t current_timestamp_ms,
	struct contact_info_list **removed_contacts)
{
	/* Check for ending contacts */
	/* We do this first as it frees space in the list */
	int8_t removed = 0;
	struct contact_info_list *cur = ctx->current_contacts;
	struct contact_info_list *prev = NULL;

	while (cur) {
		const struct contact_info ci = cur->contact_info;

		if (ci.to_ms <= current_timestamp_ms) {
			// Keep pointer to next element
			struct contact_info_list *const next = cur->next;

			// Add entry to removed_contacts
			cur->next = *removed_contacts;
			*removed_contacts = cur;

			ctx->current_contact_count--;
			// Remove entry from ctx->current_contacts
			if (!prev) // remove head
				ctx->current_contacts = next;
			else // remove middle or tail
				prev->next = next;

			cur = next;
		} else {
			prev = cur;
			cur = cur->next;
		}
	}

	return removed;
}

static uint8_t check_upcoming(
	const struct contact_manager_context *const ctx,
	struct contact *c,
	struct contact_info_list **added_contacts)
{
	// Contact is already active, do nothing
	if (contact_active(ctx, c))
		return 0;

	// Too many contacts are already active, cannot add another...
	if (ctx->current_contact_count >= MAX_CONCURRENT_CONTACTS) {
		LOGF_WARN(
			"ContactManager: Cannot start contact with \"%s\", too many contacts are already active",
			c->node->eid
		);
		return 0;
	}

	/* Add contact */
	struct contact_info ci;

	ci.eid = strdup(c->node->eid);
	if (!ci.eid) {
		LOG_WARN("ContactManager: Failed to copy EID");
		return 0;
	}
	ci.cla_addr = strdup(c->node->cla_addr);
	if (!ci.cla_addr) {
		LOG_WARN("ContactManager: Failed to copy CLA address");
		free(ci.eid);
		return 0;
	}
	ci.to_ms = c->to_ms;

	// Add to the list of added contacts
	struct contact_info_list *const added_entry = malloc(sizeof(struct contact_info_list));

	if (!added_entry) {
		LOG_WARN("ContactManager: Failed to allocate contact_info_list entry");
		free(ci.eid);
		free(ci.cla_addr);
		return 0;
	}
	added_entry->contact_info = ci;
	added_entry->next = *added_contacts;
	*added_contacts = added_entry;

	return 1;
}

static int8_t process_upcoming_list(
	struct contact_manager_context *const ctx,
	struct contact_list *contact_list,
	const uint64_t current_timestamp_ms,
	struct contact_info_list **added_contacts)
{
	int8_t added = 0;
	struct contact_list *cur_entry;

	ctx->next_contact_time_ms = UINT64_MAX;
	cur_entry = contact_list;
	while (cur_entry != NULL) {
		if (cur_entry->data->from_ms <= current_timestamp_ms) {
			if (cur_entry->data->to_ms > current_timestamp_ms) {
				added += check_upcoming(
					ctx,
					cur_entry->data,
					added_contacts
				);
				if (cur_entry->data->to_ms <
				    ctx->next_contact_time_ms)
					ctx->next_contact_time_ms =
						cur_entry->data->to_ms;
			}
		} else {
			if (cur_entry->data->from_ms <
			    ctx->next_contact_time_ms)
				ctx->next_contact_time_ms = (
					cur_entry->data->from_ms
				);
			/* As our contact_list is sorted ascending by */
			/* from-time we can stop checking here */
			break;
		}
		cur_entry = cur_entry->next;
	}
	return added;
}

static void check_for_contacts(
	struct contact_manager_context *const ctx,
	struct contact_list **contact_list,
	Semaphore_t semphr, QueueIdentifier_t bp_queue)
{
	struct contact_info_list *added_contacts = NULL;
	struct contact_info_list *removed_contacts = NULL;
	const uint64_t current_timestamp_ms = hal_time_get_timestamp_ms();

	ASSERT(semphr != NULL);
	ASSERT(bp_queue != NULL);

	hal_semaphore_take_blocking(semphr);

	remove_expired_contacts(
		ctx,
		current_timestamp_ms,
		&removed_contacts
	);
	process_upcoming_list(
		ctx,
		*contact_list, // dereference only when we have the semaphore!
		current_timestamp_ms,
		&added_contacts
	);

	hal_semaphore_release(semphr);
	ASSERT(ctx->next_contact_time_ms > current_timestamp_ms);

	// Process added contacts
	struct contact_info_list *cur_added = added_contacts;

	while (cur_added) {
		const struct contact_info ci = cur_added->contact_info;

		LOGF_INFO(
			"ContactManager: Scheduled contact with \"%s\" started.",
			ci.eid
		);

		struct fib_request *req = malloc(
			sizeof(struct fib_request)
		);

		if (req) {
			req->type = FIB_REQUEST_CREATE_LINK;
			req->flags = FIB_ENTRY_FLAG_NONE;
			req->node_id = strdup(ci.eid);
			req->cla_addr = strdup(ci.cla_addr);

			bundle_processor_inform(
				bp_queue,
				(struct bundle_processor_signal){
					.type = BP_SIGNAL_FIB_UPDATE_REQUEST,
					.fib_request = req,
				}
			);
		}

		// Add to the list of all contacts
		struct contact_info_list *const next = cur_added->next;

		cur_added->next = ctx->current_contacts;
		ctx->current_contacts = cur_added;
		ctx->current_contact_count++;

		cur_added = next;

		// A memory leak is wrongly indicated for `node_id` and
		// `cla_addr`, which are passed in `req` through the queue.
		// cppcheck-suppress memleak
	}

	// Process removed contacts
	struct contact_info_list *cur_removed = removed_contacts;

	while (cur_removed) {
		struct contact_info ci = cur_removed->contact_info;

		LOGF_INFO(
			"ContactManager: Scheduled contact with \"%s\" ended.",
			ci.eid
		);

		struct fib_request *req = malloc(
			sizeof(struct fib_request)
		);

		if (req) {
			req->type = FIB_REQUEST_DROP_LINK;
			req->flags = FIB_ENTRY_FLAG_NONE;
			req->node_id = ci.eid;
			req->cla_addr = ci.cla_addr;

			bundle_processor_inform(
				bp_queue,
				(struct bundle_processor_signal){
					.type = BP_SIGNAL_FIB_UPDATE_REQUEST,
					.fib_request = req,
				}
			);
		} else {
			free(ci.eid);
			free(ci.cla_addr);
		}

		ci.eid = NULL;
		ci.cla_addr = NULL;

		struct contact_info_list *const tmp = cur_removed;

		cur_removed = cur_removed->next;
		free(tmp);
	}
}

static void contact_manager_task(void *cm_parameters)
{
	struct contact_manager_task_parameters *parameters =
		(struct contact_manager_task_parameters *)cm_parameters;
	int signal;
	struct contact_manager_context ctx = {
		.current_contact_count = 0,
		.next_contact_time_ms = UINT64_MAX,
	};

	if (!parameters) {
		LOG_ERROR("ContactManager: Cannot start, parameters not defined");
		abort();
	}
	for (;;) {
		check_for_contacts(
			&ctx,
			parameters->contact_list_ptr,
			parameters->semaphore,
			parameters->bp_queue
		);

		const uint64_t cur_time_ms = hal_time_get_timestamp_ms();
		int64_t delay_ms = -1; // infinite blocking on queue

		if (ctx.next_contact_time_ms < UINT64_MAX) {
			const uint64_t next_time_ms = ctx.next_contact_time_ms;

			if (next_time_ms <= cur_time_ms)
				continue;

			const uint64_t udelay = next_time_ms - cur_time_ms + 1;

			// The queue implementation does not support to wait
			// longer than 292 years due to a conversion to
			// nanoseconds. Block indefinitely in this case.
			if (udelay < HAL_QUEUE_MAX_DELAY_MS &&
					udelay <= (uint64_t)INT64_MAX)
				delay_ms = udelay;
		}
		hal_queue_receive(
			parameters->control_queue,
			&signal,
			delay_ms
		);
	}
}

struct contact_manager_params contact_manager_start(
	QueueIdentifier_t bp_queue,
	struct contact_list **clistptr)
{
	struct contact_manager_params ret = {
		.task_creation_result = UD3TN_FAIL,
		.semaphore = NULL,
		.control_queue = NULL,
	};
	QueueIdentifier_t queue;
	struct contact_manager_task_parameters *cmt_params;
	Semaphore_t semaphore = hal_semaphore_init_binary();

	if (semaphore == NULL)
		return ret;
	hal_semaphore_release(semaphore);
	queue = hal_queue_create(1, sizeof(int));
	if (queue == NULL) {
		hal_semaphore_delete(semaphore);
		return ret;
	}
	cmt_params = malloc(sizeof(struct contact_manager_task_parameters));
	if (cmt_params == NULL) {
		hal_semaphore_delete(semaphore);
		hal_queue_delete(queue);
		return ret;
	}
	cmt_params->semaphore = semaphore;
	cmt_params->control_queue = queue;
	cmt_params->bp_queue = bp_queue;
	cmt_params->contact_list_ptr = clistptr;

	ret.task_creation_result = hal_task_create(
		contact_manager_task,
		cmt_params,
		true,
		NULL
	);
	if (ret.task_creation_result == UD3TN_OK) {
		ret.semaphore = semaphore;
		ret.control_queue = queue;
	} else {
		hal_semaphore_delete(semaphore);
		hal_queue_delete(queue);
	}
	return ret;
}
