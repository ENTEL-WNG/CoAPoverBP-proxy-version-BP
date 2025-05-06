// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef CLA_H_INCLUDED
#define CLA_H_INCLUDED

#include "cla/cla_contact_rx_task.h"

#include "ud3tn/bundle.h"
#include "ud3tn/bundle_processor.h"
#include "ud3tn/crc.h"
#include "ud3tn/result.h"

#include "spp/spp_timecodes.h"

#include "platform/hal_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum number of CLA config options. If any new CLA needs more, increase.
#define CLA_MAX_OPTION_COUNT 10

// Length of the outgoing-bundle queue.
#ifndef CONTACT_TX_TASK_QUEUE_LENGTH
#define CONTACT_TX_TASK_QUEUE_LENGTH 100
#endif // CONTACT_TX_TASK_QUEUE_LENGTH

enum cla_begin_packet_result {
	CLA_BEGIN_PACKET_OK = 0,
	CLA_BEGIN_PACKET_DONE = 1,
	CLA_BEGIN_PACKET_FAIL = -1,
};

// Result of start and end contact functions
enum cla_link_update_result {
	// The update attempt failed.
	CLA_LINK_UPDATE_FAILED = -1,
	// Start contact: the link is already active.
	// End contact: the link was already down.
	CLA_LINK_UPDATE_UNCHANGED = 0,
	// The update was triggered and will be performed asynchronously.
	CLA_LINK_UPDATE_INITIATED = 1,
	// The update was successfully performed synchronously.
	CLA_LINK_UPDATE_PERFORMED = 2,
	// End contact: the link will continue as opportunistic.
	CLA_LINK_UPDATE_IGNORED = 3,
};

struct cla_config {
	const struct cla_vtable *vtable;

	const struct bundle_agent_interface *bundle_agent_interface;
};

struct cla_link {
	struct cla_config *config;

	// CLA address (without the CLA identifier) related to the link
	char *cla_addr;

	// Semaphore for waiting until the RX task is finished
	Semaphore_t rx_task_sem;
	// Semaphore for waiting until the TX task is finished
	Semaphore_t tx_task_sem;

	struct rx_task_data rx_task_data;
	// Semaphore for notifying the RX task to finish
	Semaphore_t rx_task_notification;

	// Timestamp of last received byte for implementing the read timeout
	uint64_t last_rx_time_ms;

	// Queue handing over bundles to the TX task
	QueueIdentifier_t tx_queue_handle;
	// Semaphore blocking the TX queue while bundles are being added
	Semaphore_t tx_queue_sem;
};

struct cla_tx_queue {
	QueueIdentifier_t tx_queue_handle;
	Semaphore_t tx_queue_sem;
};

/*
 * Global CLA Instance Management
 */

enum ud3tn_result cla_initialize_all(
	const char *cla_config_str,
	const struct bundle_agent_interface *bundle_agent_interface);

void cla_terminate_all(void);

struct cla_config *cla_config_get(const char *cla_addr);

uint64_t cla_get_max_bundle_size_bytes(void);

/*
 * Private API
 */

enum ud3tn_result cla_config_init(
	struct cla_config *config,
	const struct bundle_agent_interface *bundle_agent_interface);

enum ud3tn_result cla_link_init(struct cla_link *link,
				struct cla_config *config,
				const char *const cla_addr,
				bool is_rx, bool is_tx);

// Wait for the link tasks to terminate and, then, delete all associated data.
void cla_link_wait_cleanup(struct cla_link *link);
// Wait for graceful termination of RX and TX task.
void cla_link_wait(struct cla_link *link);
// Delete the data associated with the cla_link.
void cla_link_cleanup(struct cla_link *link);

char *cla_get_connect_addr(const char *cla_addr, const char *cla_name);

void cla_generic_disconnect_handler(struct cla_link *link);

// Constructs the CLA address (`<cla>:<addr>`) from the connected link address.
char *cla_get_cla_addr_from_link(const struct cla_link *link);

/*
 * Virtual Functions
 */

struct cla_vtable {
	// Public API

	/* Returns a unique identifier of the CLA as part of the CLA address */
	const char *(*cla_name_get)(void);

	/* Starts the TX/RX tasks and, e.g., the socket listener */
	enum ud3tn_result (*cla_launch)(struct cla_config *);
	/* Stops the TX/RX tasks and, e.g., the socket listener */
	enum ud3tn_result (*cla_terminate)(struct cla_config *);

	/* Obtains the max. serialized size of outgoing bundles for this CLA. */
	size_t (*cla_mbs_get)(struct cla_config *);

	/* Returns the transmission queue for the given node EID and address */
	struct cla_tx_queue (*cla_get_tx_queue)(struct cla_config *,
						const char *, const char *);
	/* Initiates a scheduled contact for a given EID and CLA address */
	enum cla_link_update_result (*cla_start_scheduled_contact)(
		struct cla_config *,
		const char *,
		const char *
	);
	/* Ends a scheduled contact for a given EID and CLA address */
	enum cla_link_update_result (*cla_end_scheduled_contact)(
		struct cla_config *,
		const char *,
		const char *
	);

	// TX Task API

	/* Initiates bundle transmission for a single bundle */
	enum cla_begin_packet_result (*cla_begin_packet)(struct cla_link *,
				 const struct bundle * const,
				 size_t,
				 char *);
	/* Terminates bundle transmission for a single bundle */
	enum ud3tn_result (*cla_end_packet)(struct cla_link *);
	/* Sends part of the serialized bundle. Can be called multiple times. */
	enum ud3tn_result (*cla_send_packet_data)(struct cla_link *,
				     const void *,
				     const size_t);

	// RX Task API

	void (*cla_rx_task_reset_parsers)(struct cla_link *);
	size_t (*cla_rx_task_forward_to_specific_parser)(struct cla_link *,
							 const uint8_t *,
							 size_t);

	/* Reads a chunk of data */
	enum ud3tn_result (*cla_read)(struct cla_link *, uint8_t *buffer,
				      size_t length, size_t *bytes_read);

	/* Cleans up resources after a link broke */
	void (*cla_disconnect_handler)(struct cla_link *);
};

#endif /* CLA_H_INCLUDED */
