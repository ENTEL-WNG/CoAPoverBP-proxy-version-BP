// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef SIMPLEHTAB_H_INCLUDED
#define SIMPLEHTAB_H_INCLUDED

#include <stdint.h>
#include <stddef.h>

/**
 * The data structure used for managing a slot of the hash table. Each slot
 * contains a linked list to which elements are added in the order of insertion.
 */
struct htab_entrylist {
	char *key;
	void *value;
	struct htab_entrylist *next;
};

/**
 * The data structure used for managing the hash table. May be used for
 * allocating an instance statically, in conjunction with allocating a
 * corresponding struct htab_entrylist[] array.
 */
struct htab {
	uint16_t slot_count;
	struct htab_entrylist **elements;
};

/**
 * Initializes a statically-allocated hash table instance.
 *
 * @param tab A pointer to the statically-allocated data structure.
 * @param slot_count The number of slots. Must match the number of elements in
 *                   entrylist[].
 * @param entrylist A pointer to the static array for the table entries.
 */
void htab_init(struct htab *tab, const uint16_t slot_count,
	       struct htab_entrylist *entrylist[]);

/**
 * Allocates a new hash table instance dynamically.
 *
 * @param slot_count The number of slots the hash table shall consist of.
 *
 * @return The hash table structure, or NULL on error.
 */
struct htab *htab_alloc(uint16_t slot_count);

/**
 * Removes (and de-allocates) all elements of the hash table. Usable for
 * statically-allocated instances as well.
 *
 * @param tab A pointer to the hash table data structure.
 */
void htab_trunc(struct htab *tab);

/**
 * De-allocates a dynamically-allocated hash table. Must not be used for
 * statically-allocated instances.
 *
 * @param tab A pointer to the hash table data structure.
 */
void htab_free(struct htab *tab);

/**
 * Adds an element to the hash table. If the given key exists already, the
 * element is not added and NULL is returned.
 *
 * @param tab A pointer to the hash table data structure.
 * @param key A C string to be used as key. Will be copied internally.
 * @param valptr A pointer to the value to be added. Will not be accessed or
 *               modified by the hash table and needs to be always valid while
 *               it is contained in it.
 *
 * @return A pointer to the list entry data structure that has been added, or
 *         NULL on error.
 */
struct htab_entrylist *htab_add(
	struct htab *tab, const char *key, void *valptr);

/**
 * Updates an element in the hash table and returns the previous value.
 * If the element does not exist, it is added to the hash table.
 *
 * @param tab A pointer to the hash table data structure.
 * @param key A C string to be used as key. Will be copied internally.
 * @param valptr A pointer to the value to be added. Will not be accessed or
 *               modified by the hash table and needs to be always valid while
 *               it is contained in it.
 * @param prev Used to return the pointer to the previous value. Will not be
 *             modified if NULL.
 *
 * @return A pointer to the list entry data structure that has been updated, or
 *         NULL on error.
 */
struct htab_entrylist *htab_update(
	struct htab *tab, const char *key, void *valptr, void **prev);

/**
 * Gets the value stored for the given key from the hash table.
 *
 * @param tab A pointer to the hash table data structure.
 * @param key A C string representing the key.
 *
 * @return A pointer to the stored value, or NULL if it is not found.
 */
void *htab_get(struct htab *tab, const char *key);

/**
 * Gets the entry data structure stored for the given key from the hash table.
 *
 * @param tab A pointer to the hash table data structure.
 * @param key A C string representing the key.
 *
 * @return A pointer to the stored data structure, or NULL if it is not found.
 */
struct htab_entrylist *htab_get_pair(struct htab *tab, const char *key);

/**
 * Removes the value stored for the given key from the hash table.
 *
 * @param tab A pointer to the hash table data structure.
 * @param key A C string representing the key.
 *
 * @return A pointer to the removed value, or NULL if it was not found.
 */
void *htab_remove(struct htab *tab, const char *key);

/* -- INTERNAL FUNCTIONS -- */

/**
 * Adds an element with a known hash to the hash table. INTERNAL FUNCTION;
 * NOT TO BE USED by user code in most circumstances.
 */
struct htab_entrylist *htab_add_known(
	struct htab *tab, const char *key, const uint16_t hash,
	const size_t key_length, void *valptr,
	const uint8_t compare_ptr_only);

/**
 * Updates an element with a known hash in the hash table and returns the
 * previous value. INTERNAL FUNCTION; NOT TO BE USED by user code in most
 * circumstances.
 */
struct htab_entrylist *htab_update_known(
	struct htab *tab, const char *key, const uint16_t hash,
	const size_t key_length, void *valptr, void **prev,
	const uint8_t compare_ptr_only);

/**
 * Gets the value stored for a given key with a known hash from the hash table.
 * INTERNAL FUNCTION; NOT TO BE USED by user code in most circumstances.
 */
void *htab_get_known(
	struct htab *tab, const char *key, const uint16_t hash,
	const uint8_t compare_ptr_only);

/**
 * Gets the entry data structure stored for a given key with a known hash from
 * the hash table. INTERNAL FUNCTION; NOT TO BE USED by user code in most
 * circumstances.
 */
struct htab_entrylist *htab_get_known_pair(
	struct htab *tab, const char *key, const uint16_t hash,
	const uint8_t compare_ptr_only);

/**
 * Removes a value with a known hash from the hash table. INTERNAL FUNCTION;
 * NOT TO BE USED by user code in most circumstances.
 */
void *htab_remove_known(
	struct htab *tab, const char *key, const uint16_t hash,
	const uint8_t compare_ptr_only);

#endif // SIMPLEHTAB_H_INCLUDED
