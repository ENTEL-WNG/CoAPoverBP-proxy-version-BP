// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FIB_H_INCLUDED
#define FIB_H_INCLUDED

#ifndef FIB_NODE_HTAB_SLOT_COUNT
#define FIB_NODE_HTAB_SLOT_COUNT 32
#endif // FIB_NODE_HTAB_SLOT_COUNT

#ifndef FIB_CLA_ADDR_HTAB_SLOT_COUNT
#define FIB_CLA_ADDR_HTAB_SLOT_COUNT 64
#endif // FIB_CLA_ADDR_HTAB_SLOT_COUNT

#include <stdbool.h>

/**
 * Defines possible values for the status of a CLA Link.
 * If a Link is stored in the FIB, it can be either UNSPECIFIED (waiting for
 * BPA and CLA to confirm its presence and availability), UP (can be used),
 * or DOWN (confirmed unavailable by the CLA).
 * In requests to change a Link's status, UNSPECIFIED indicates that the given
 * Link's status shall not be changed (if the change request indicates UP,
 * the BPA shall try to establish the Link or try to establish it again;
 * if it indicates DOWN, the BPA shall stop using the link and start the
 * process of removing it and all associated resources).
 */
enum fib_link_status {
	// The Link is known but not usable (of unknown status or not
	// established) or (if specified in FIB change requests) its current
	// status should not be changed.
	FIB_LINK_STATUS_UNSPECIFIED = 0,
	// The Link is established via the CLA.
	FIB_LINK_STATUS_UP = 1,
	// The Link is unavailable and cannot be used.
	FIB_LINK_STATUS_DOWN = 2,
};

/**
 * Additional properties/flags that can be assigned to a FIB entry to alter the
 * behavior of the internal forwarding logic.
 */
enum fib_entry_flags {
	// No additional flag assigned to the entry.
	FIB_ENTRY_FLAG_NONE = 0,
	// The entry is directly usable, without BDM interaction/confirmation.
	FIB_ENTRY_FLAG_DIRECT = 1,
};

/**
 * A data structure containing the status of a CLA link that is always
 * associated with a single CLA address and updated based on it.
 */
struct fib_link {
	// The CLA address for the Link.
	const char *cla_addr;
	// The current status of the Link.
	enum fib_link_status status;
	// The list of node IDs, i.e., FIB entries, which refer to that Link.
	struct fib_link_reflist {
		const char *node_id;
		struct fib_link_reflist *next;
	} *reflist;
};

/**
 * The data structure used for managing an entry in the FIB.
 * A FIB entry is always associated with a single node ID. If multiple nodes
 * point to the same CLA address, this means that multiple FIB entries contain
 * the same CLA address.
 */
struct fib_entry {
	const char *cla_addr;
	enum fib_entry_flags flags;
};

/**
 * Allocates a new FIB instance dynamically.
 *
 * @return The new FIB instance, or NULL on error.
 */
struct fib *fib_alloc(void);

/**
 * De-allocates a dynamically-allocated FIB instance.
 *
 * @param fib A valid and usable FIB instance to be freed.
 */
void fib_free(struct fib *fib);

/**
 * Obtains the FIB entry stored for the given node ID.
 *
 * @param fib A pointer to the FIB data structure.
 * @param node_id The node ID that shall be looked up.
 *
 * @return The FIB entry defining the associated Link and its status, or NULL
 *         if no such entry was found.
 */
struct fib_entry *fib_lookup_node(struct fib *fib,
				  const char *node_id);

/**
 * Obtains the FIB entry containing a Link with the given CLA address.
 *
 * @param fib A pointer to the FIB data structure.
 * @param cla_addr The CLA address that shall be looked up.
 *
 * @return The FIB entry defining the associated Link and its status, or NULL
 *         if no such entry was found.
 */
struct fib_link *fib_lookup_cla_addr(struct fib *fib,
				     const char *cla_addr);

/**
 * Adds a Link with the given CLA address to the FIB. Note that this does not
 * add a forwarding entry (a mapping from a node to the given Link) but just
 * creates a Link entry with status UNSPECIFIED. If an entry exists already,
 * it is not changed and a pointer to it is returned.
 *
 * @param fib A pointer to the FIB data structure.
 * @param cla_addr The CLA address for which a Link entry shall be created. Will
 *                 be copied internally by the hash table implementation.
 *
 * @return A pointer to the newly-created Link entry or the one that did already
 *         exist, or NULL on error (if no entry could be created).
 */
struct fib_link *fib_insert_link(struct fib *fib,
				 const char *cla_addr);

/**
 * Adds a forwarding entry, i.e., a mapping for the given node ID to a Link
 * with the given CLA address, to the FIB. If no Link has been recorded
 * previously for the given CLA address, a new Link entry is created with
 * status UNSPECIFIED. If a forwarding entry did already exist, it is updated
 * to point to the Link with the specified CLA address (at the moment, multiple
 * concurrent Links for a single node are not supported)
 *
 * @param fib A pointer to the FIB data structure.
 * @param node_id The node ID for which a forwarding entry shall be created.
 *                Will be copied internally by the hash table implementation.
 * @param cla_addr The CLA address to which data that have the given node ID as
 *                 next hop shall be forwarded. Will be copied internally by
 *                 the hash table implementation.
 *
 * @return A pointer to the Link entry that the new or updated node association
 *         points to, or NULL on error (if no entry could be created).
 */
struct fib_entry *fib_insert_node(struct fib *fib,
				  const char *node_id,
				  const char *cla_addr);

/**
 * Removes any existing forwarding entry (but no Link definition(s)) for the
 * given node ID and returns whether an entry was found.
 *
 * @param fib A pointer to the FIB data structure.
 * @param node_id The node ID for which forwarding entries shall be removed.
 * @param drop_all_associated Delete all other node associations for the Link.
 *
 * @return true if an entry was found and has been deleted, otherwise false.
 */
bool fib_remove_node(struct fib *const fib, const char *const node_id,
		     bool drop_all_associated);

/**
 * Removes any existing Link definition for the given CLA address and returns
 * whether the action was successful. The reference count has to be zero for
 * this to work.
 *
 * @param fib A pointer to the FIB data structure.
 * @param cla_addr The CLA address for which a Link shall be removed.
 *
 * @return true if a Link was found and has been deleted, otherwise false.
 */
bool fib_remove_link(struct fib *fib, const char *cla_addr);

/**
 * A callback function type that can be passed to fib_foreach() to iterate
 * through the FIB.
 *
 * @param context User-defined data that is passed through by fib_foreach().
 * @param node_id A pointer to the node ID that is stored in the FIB as the key
 *                of the current entry.
 * @param entry A pointer to the FIB entry stored for the specified node ID,
 *              which contains the corresponding flags and CLA address.
 * @param link A pointer to the Link definition that is stored in the FIB as
 *             the value of the current entry.
 *
 * @return true if the iteration shall continue, false to stop it prematurely.
 */
typedef bool fib_iter_fun(
	void *context,
	const char *node_id,
	const struct fib_entry *entry,
	const struct fib_link *link);

/**
 * Iterates through the list of forwarding entries contained in the FIB and
 * invokes the specified callback function for each entry.
 *
 * @param fib A pointer to the FIB data structure.
 * @param callback The callback function to be invoked for each entry.
 * @param context User-defined data that is passed to the callback function.
 * @param cla_addr_filter Only iterate through entries matching the given
 *                        CLA address. If NULL, iterate through the whole FIB.
 */
void fib_foreach(struct fib *fib, fib_iter_fun *callback, void *context,
		 const char *cla_addr_filter);

#endif // FIB_H_INCLUDED
