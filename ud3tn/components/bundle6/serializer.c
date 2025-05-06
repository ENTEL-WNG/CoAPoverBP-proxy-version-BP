// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "bundle6/sdnv.h"
#include "bundle6/bundle6.h"
#include "bundle6/serializer.h"

#include "ud3tn/result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define write_bytes(success_var, bytes, data) do { \
	if ((success_var) && write(cla_obj, data, bytes) != UD3TN_OK) \
		(success_var) = false; \
} while (0)
#define serialize_u16(success_var, buffer, value) do { \
	__typeof__(buffer) *_buffer = &(buffer); \
	write_bytes(success_var, sdnv_write_u16(*_buffer, value), *_buffer); \
} while (0)
#define serialize_u32(success_var, buffer, value) do { \
	__typeof__(buffer) *_buffer = &(buffer); \
	write_bytes(success_var, sdnv_write_u32(*_buffer, value), *_buffer); \
} while (0)
#define serialize_u64(success_var, buffer, value) do { \
	__typeof__(buffer) *_buffer = &(buffer); \
	write_bytes(success_var, sdnv_write_u64(*_buffer, value), *_buffer); \
} while (0)

enum ud3tn_result bundle6_serialize(
	struct bundle *bundle,
	enum ud3tn_result (*write)(void *cla_obj, const void *, const size_t),
	void *cla_obj)
{
	uint8_t buffer[MAX_SDNV_SIZE];

	// Serialize dict
	struct bundle6_dict_descriptor *dict_desc = bundle6_calculate_dict(
		bundle
	);
	char *dict = malloc(dict_desc->dict_length_bytes);

	if (dict == NULL) {
		free(dict_desc);
		return UD3TN_FAIL;
	}

	bundle6_serialize_dictionary(dict, dict_desc);

	// Indicates whether the serialization process can continue
	bool ok = true;

	/* Write version field */
	write_bytes(ok, 1, &(bundle->protocol_version));
	/* Write SDNV values up to dictionary length */
	serialize_u32(ok, buffer, bundle->proc_flags & (
		/* Only RFC 5050 flags */
		BUNDLE_FLAG_IS_FRAGMENT
		| BUNDLE_FLAG_ADMINISTRATIVE_RECORD
		| BUNDLE_FLAG_MUST_NOT_BE_FRAGMENTED
		| BUNDLE_V6_FLAG_CUSTODY_TRANSFER_REQUESTED
		| BUNDLE_V6_FLAG_SINGLETON_ENDPOINT
		| BUNDLE_FLAG_ACKNOWLEDGEMENT_REQUESTED
		| BUNDLE_V6_FLAG_NORMAL_PRIORITY
		| BUNDLE_V6_FLAG_EXPEDITED_PRIORITY
		| BUNDLE_FLAG_REPORT_RECEPTION
		| BUNDLE_V6_FLAG_REPORT_CUSTODY_ACCEPTANCE
		| BUNDLE_FLAG_REPORT_FORWARDING
		| BUNDLE_FLAG_REPORT_DELIVERY
		| BUNDLE_FLAG_REPORT_DELETION
	));
	serialize_u32(ok, buffer, bundle->primary_block_length);
	serialize_u16(ok, buffer,
		      dict_desc->destination_eid_info.dict_scheme_offset);
	serialize_u16(ok, buffer,
		      dict_desc->destination_eid_info.dict_ssp_offset);
	serialize_u16(ok, buffer,
		      dict_desc->source_eid_info.dict_scheme_offset);
	serialize_u16(ok, buffer,
		      dict_desc->source_eid_info.dict_ssp_offset);
	serialize_u16(ok, buffer,
		      dict_desc->report_to_eid_info.dict_scheme_offset);
	serialize_u16(ok, buffer,
		      dict_desc->report_to_eid_info.dict_ssp_offset);
	serialize_u16(ok, buffer,
		      dict_desc->custodian_eid_info.dict_scheme_offset);
	serialize_u16(ok, buffer,
		      dict_desc->custodian_eid_info.dict_ssp_offset);
	serialize_u64(ok, buffer, bundle->creation_timestamp_ms / 1000);
	serialize_u64(ok, buffer, bundle->sequence_number);
	serialize_u64(ok, buffer, bundle->lifetime_ms / 1000);
	serialize_u16(ok, buffer, dict_desc->dict_length_bytes);
	/* Write dictionary byte array */
	write_bytes(ok, dict_desc->dict_length_bytes, dict);
	/* Write remaining SDNV values */
	if (HAS_FLAG(bundle->proc_flags, BUNDLE_FLAG_IS_FRAGMENT)) {
		serialize_u64(ok, buffer, bundle->fragment_offset);
		serialize_u64(ok, buffer, bundle->total_adu_length);
	}

	/* Serialize bundle blocks */
	struct bundle_block_list *cur_entry = bundle->blocks;
	struct endpoint_list *cur_ref;
	int eid_idx = 0;

	while (ok && cur_entry != NULL) {
		write_bytes(ok, 1, &cur_entry->data->type);
		serialize_u32(ok, buffer, cur_entry->data->flags);
		if (HAS_FLAG(cur_entry->data->flags,
			BUNDLE_V6_BLOCK_FLAG_HAS_EID_REF_FIELD)
		) {
			// Determine the count of refs
			int eid_ref_cnt = 0;

			for (cur_ref = cur_entry->data->eid_refs; cur_ref;
			     cur_ref = cur_ref->next, eid_ref_cnt++)
				;
			serialize_u16(ok, buffer, eid_ref_cnt);
			// Write out the refs
			for (int c = 0; c < eid_ref_cnt; c++, eid_idx++) {
				struct bundle6_eid_info eid_info =
					dict_desc->eid_references[c];
				serialize_u16(ok, buffer,
					      eid_info.dict_scheme_offset);
				serialize_u16(ok, buffer,
					      eid_info.dict_ssp_offset);
			}
		}
		serialize_u64(ok, buffer, cur_entry->data->length);
		write_bytes(ok, cur_entry->data->length, cur_entry->data->data);
		cur_entry = cur_entry->next;
	}

	free(dict);
	free(dict_desc);
	return ok ? UD3TN_OK : UD3TN_FAIL;
}
