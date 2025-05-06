// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef RTCOMPAT_CONTACTMANAGER_H_INCLUDED
#define RTCOMPAT_CONTACTMANAGER_H_INCLUDED

#include "ud3tn/common.h"

#include "routing/compat/node.h"

#include "platform/hal_types.h"

#include <stdint.h>

// Compat. router: Maximum number of concurrent contacts that can be handled by
// the Contact Manager.
#ifndef ROUTER_MAX_CONCURRENT_CONTACTS
#define ROUTER_MAX_CONCURRENT_CONTACTS 10
#endif // ROUTER_MAX_CONCURRENT_CONTACTS

struct contact_manager_params {
	enum ud3tn_result task_creation_result;
	Semaphore_t semaphore;
	QueueIdentifier_t control_queue;
};

struct contact_manager_params contact_manager_start(
	QueueIdentifier_t bp_queue,
	struct contact_list **clistptr);

#endif // RTCOMPAT_CONTACTMANAGER_H_INCLUDED
