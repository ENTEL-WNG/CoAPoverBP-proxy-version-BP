#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# encoding: utf-8

"""Minimal Bundle Dispatcher service with static routing for uD3TN-NG."""

import argparse
import asyncio
import dataclasses
import logging
import re
import sys
import time

from typing import Dict, List, Optional, Set, Tuple

from google.protobuf.internal import encoder as proto_encoder

from pyd3tn.eid import get_node_id

from ud3tn_utils.aap2 import (
    AAP2AsyncClient,
    AAP2AsyncUnixClient,
    AAP2AsyncTCPClient,
    AAP2ServerDisconnected,
    AAPMessage,
    AAPResponse,
    AuthType,
    Bundle,
    BundleADU,
    BundleADUFlags,
    BundleDispatchInfo,
    DispatchReason,
    DispatchEvent,
    DispatchResult,
    Link,
    LinkStatus,
    ResponseStatus,
)
from ud3tn_utils.config import (
    UNIX_TO_DTN_OFFSET,
)
from ud3tn_utils.storage_agent import (
    CompoundBundleId,
    StorageCall,
    StorageOperation,
)
from ud3tn_utils.aap2.bin.helpers import (
    add_common_parser_arguments,
    add_keepalive_parser_argument,
    get_secret_from_args,
    initialize_logger,
    DEFAULT_CONFIG_AGENT_ID_DTN,
    DEFAULT_CONFIG_AGENT_ID_IPN,
)

CONFIG_PARSE_REGEX = re.compile(
    r"^(?P<cmd>[1-3])\((?P<nodeid>[^\)]+)\)(\:(\((?P<claaddr>[^\)]+)\))?"
    r"(\:(?P<eidlist>\[(\(([^\)]+)\),?)*\])?"
    r"(\:(?P<contactlist>\[(\{[0-9]+,[0-9]+,[0-9]+(,\[(\([^\)]+\),?)+\])?\}"
    r",?)*\])?)?)?)?;$"
)
EID_LIST_PARSE_REGEX = re.compile(
    r"^[\[,]\((?P<eid>[^\)]+)\)(?P<remainder>,\(.+\))?\]?$"
)
CONTACT_LIST_PARSE_REGEX = re.compile(
    r"^[\[,]\{(?P<start>[0-9]+),(?P<end>[0-9]+),(?P<datarate>[0-9]+)"
    r"(,(?P<eidlist>\[(\([^\)]+\),?)+\]))?\}(?P<remainder>,\{.+\})?\]?$"
)

# uD3TN config protocol operations
OP_ADD = 1
OP_REPLACE = 2
OP_DELETE = 3

# Which DispatchReason values we accept for new bundles to be scheduled
# (in other cases they are dropped).
VALID_SCHEDULE_DISPATCH_REASONS = (
    DispatchReason.DISPATCH_REASON_NO_FIB_ENTRY,
)

logger = logging.getLogger(__name__)


@dataclasses.dataclass(frozen=True)
class ContactDefinition:
    """Represents a planned contact"""
    start: int
    end: int
    next_hop_node_id: str
    next_hop_cla_addr: str
    data_rate: int
    # All following fields are not part of the ID
    reachable_eids: Set[str] = dataclasses.field(compare=False)


@dataclasses.dataclass(frozen=True)
class BundleIDType:
    """Represents a bundle scheduled in the BDM"""
    src_eid: str
    creation_timestamp: int
    sequence_number: int
    fragment_offset: int
    payload_length: int
    # All following fields are not part of the ID
    dst_eid: str = dataclasses.field(compare=False)
    min_frag_size_first: int = dataclasses.field(compare=False)
    min_frag_size_last: int = dataclasses.field(compare=False)

    def to_proto(self):
        return CompoundBundleId(
            source_eid=self.src_eid,
            creation_timestamp=self.creation_timestamp,
            sequence_number=self.sequence_number,
            fragment_offset=self.fragment_offset,
            payload_length=self.payload_length,
        )


SchedDBType = Dict[
    ContactDefinition,
    Tuple[
        asyncio.Queue,
        Dict[BundleIDType, Tuple[int, int, int]],
        asyncio.Task,
    ],
]

FragmentDispatchTupleType = Tuple[ContactDefinition, int, int, int]
FragmentDispatchSetType = Set[FragmentDispatchTupleType]
SchedDictType = Dict[
    BundleIDType,
    FragmentDispatchSetType,
]


class ScheduleException(Exception):
    pass


class FIBLinkStatusManager:

    def __init__(self):
        self.known_links: Dict[str, asyncio.Event] = {}

    def update_link_info(self, next_hop: str, is_active: bool = False):
        # NOTE: ATM `next_hop` is a CLA addr; uD3TN does not always report EIDs
        if next_hop not in self.known_links:
            ev = asyncio.Event()
            self.known_links[next_hop] = ev
        else:
            ev = self.known_links[next_hop]
        if is_active:
            ev.set()
        else:
            ev.clear()

    def is_active(self, next_hop: str) -> bool:
        if next_hop not in self.known_links:
            return False
        return self.known_links[next_hop].is_set()

    async def wait_until_active(self, next_hop: str):
        if next_hop not in self.known_links:
            ev = asyncio.Event()
            self.known_links[next_hop] = ev
        else:
            ev = self.known_links[next_hop]
        await ev.wait()


def _parse_eid_list(eid_list: str) -> Set[str]:
    if eid_list is None:
        return set()
    match_obj = EID_LIST_PARSE_REGEX.match(eid_list)
    if match_obj is None:
        return set()
    if match_obj.group(2) is None:
        return {match_obj.group(1)}
    return {match_obj.group(1)} | _parse_eid_list(match_obj.group(2))


def _parse_contact_list(
    contact_list: str,
    next_hop_node_id: str,
    next_hop_cla_addr: str,
    reachable_eids_at_node: Set[str]
) -> Set[ContactDefinition]:
    if contact_list is None:
        return set()
    match_obj = CONTACT_LIST_PARSE_REGEX.match(contact_list)
    if match_obj is None:
        return set()
    contact = ContactDefinition(
        start=int(match_obj.group("start")),
        end=int(match_obj.group("end")),
        next_hop_node_id=next_hop_node_id,
        next_hop_cla_addr=next_hop_cla_addr,
        data_rate=int(match_obj.group("datarate")),
        reachable_eids=(
            reachable_eids_at_node
            if match_obj.group("eidlist") is None
            else (
                reachable_eids_at_node |
                _parse_eid_list(match_obj.group("eidlist"))
            )
        )
    )
    if match_obj.group("remainder") is None:
        return {contact}
    return {contact} | _parse_contact_list(
        match_obj.group("remainder"),
        next_hop_node_id,
        next_hop_cla_addr,
        reachable_eids_at_node,
    )


def _parse_config(config_str: str) -> Tuple[int, str, Set[ContactDefinition]]:
    match_obj = CONFIG_PARSE_REGEX.match(config_str)
    if match_obj is None:
        raise ValueError("invalid config string")
    return (
        int(match_obj.group("cmd")),
        match_obj.group("nodeid"),
        _parse_contact_list(
            match_obj.group("contactlist"),
            match_obj.group("nodeid"),
            match_obj.group("claaddr"),
            (
                {match_obj.group("nodeid")} |
                _parse_eid_list(match_obj.group("eidlist"))
            ),
        ),
    )


def _contact_overlaps(c1: ContactDefinition, c2: ContactDefinition) -> bool:
    return (
        c1.start < c2.end and
        c1.end > c2.start
    )


def _adjust_existing_contacts(
    global_contact_set: Set[ContactDefinition],
    next_hop_node_id: str,
    contact_set_to_add: Set[ContactDefinition]
):
    # if any of the contacts to add overlaps, remove (-> replace) the existing
    # but extend the "reachable EIDs" set in the new contact to add
    contacts_to_remove = set()
    for contact in global_contact_set:
        # the existing contact is with the same next hop as the ones to add
        if contact.next_hop_node_id == next_hop_node_id:
            for contact2 in contact_set_to_add:
                if contact != contact2 and _contact_overlaps(contact,
                                                             contact2):
                    contacts_to_remove.add(contact)
                    contact2.reachable_eids.update(contact.reachable_eids)
    return contacts_to_remove


def _decode_cfg_msg(
    global_contact_set: Set[ContactDefinition],
    adu_msg: BundleADU,
    payload: bytes,
) -> Tuple[Set[ContactDefinition], Set[ContactDefinition]]:
    operation, nh_node_id, contact_set = _parse_config(
        payload.decode("utf-8")
    )
    contacts_to_add = set()
    contacts_to_remove = set()
    if operation == OP_ADD:
        contacts_to_remove |= _adjust_existing_contacts(
            global_contact_set,
            nh_node_id,
            contact_set,
        )
        # NOTE: does not extend contacts that overlap (ud3tn v0.13 behavior)
        contacts_to_add |= contact_set
    elif operation == OP_REPLACE:
        for contact in global_contact_set:
            # remove all contacts with the node that is to be replaced
            if (contact.next_hop_node_id == nh_node_id and
                    contact not in contact_set):
                contacts_to_remove.add(contact)
        contacts_to_add |= contact_set
    elif operation == OP_DELETE:
        if len(contact_set) == 0:
            # delete all
            for contact in global_contact_set:
                if contact.next_hop_node_id == nh_node_id:
                    contacts_to_remove.add(contact)
        else:
            # delete specified contacts
            contacts_to_remove |= contact_set
    return contacts_to_add, contacts_to_remove


def _rebuild_fwd_dict(
    contact_set: Set[ContactDefinition]
) -> Dict[str, Set[ContactDefinition]]:
    fwd_dict: Dict[str, Set[ContactDefinition]] = {}
    for contact in contact_set:
        for reachable_node_id in contact.reachable_eids:
            try:
                normalized_node_id = get_node_id(reachable_node_id)
            except ValueError:
                logger.info(
                    "Cannot get node ID for EID \"%s\", skipping",
                    reachable_node_id,
                )
                continue
            if normalized_node_id not in fwd_dict:
                fwd_dict[normalized_node_id] = set()
            fwd_dict[normalized_node_id].add(contact)
    return fwd_dict


def _loop_time_to_contact_time(lt: float) -> int:
    return round(lt - UNIX_TO_DTN_OFFSET)


def _contact_time_to_loop_time(ct: int) -> int:
    return ct + UNIX_TO_DTN_OFFSET


def _get_cur_remaining_contact_capacity(
    c: ContactDefinition,
    t: float,
    capmap: Dict[ContactDefinition, Dict[BundleIDType, int]]
) -> int:
    ct = _loop_time_to_contact_time(t)
    if ct >= c.end:
        return 0
    if ct > c.start:
        total_cap = (c.end - ct) * c.data_rate
    else:
        total_cap = (c.end - c.start) * c.data_rate
    try:
        scheduled_cap = sum(capmap[c].values())
    except KeyError:
        scheduled_cap = 0
    # return max(0, total_cap - scheduled_cap)
    return int(total_cap - scheduled_cap)


# NOTE this function is also used for re-scheduling (only in this case,
# fragment_offset and fragment_length are set).
def _calc_bundle_schedule(
    fwd_dict: Dict[str, Set[ContactDefinition]],
    capmap: Dict[ContactDefinition, Dict[BundleIDType, int]],
    bid: BundleIDType,
    mbs_bytes: int,
    # allow to override the following for re-scheduling
    fragment_offset: int = 0,
    fragment_length: int = 0,
) -> FragmentDispatchSetType:
    # Main dispatch logic for a new bundle
    try:
        dst_node_id = get_node_id(bid.dst_eid)
    except ValueError:
        raise ScheduleException("cannot determine next-hop node ID")
    next_hop_contacts = fwd_dict.get(dst_node_id, None)
    if not next_hop_contacts:
        raise ScheduleException("no next hops found")
    # Try to schedule
    bundle_first_fragment_min_size_bytes = bid.min_frag_size_first
    bundle_last_fragment_min_size_bytes = bid.min_frag_size_last
    frag_remaining_payload = (
        bid.payload_length if fragment_length == 0 else fragment_length
    )
    if fragment_offset == 0:
        fragment_offset = bid.fragment_offset
    logger.debug(
        "Bundle fragmentation parameters: first_frag_min = %d, "
        "last_frag_min = %d, pl_sz = %d",
        bundle_first_fragment_min_size_bytes,
        bundle_last_fragment_min_size_bytes,
        frag_remaining_payload,
    )
    fragment_results: List[FragmentDispatchTupleType] = []
    for contact in sorted(next_hop_contacts, key=lambda c: c.start):
        cur_time = time.time()
        # Determine max. bundle size for this contact
        contact_crcap_bytes: int = _get_cur_remaining_contact_capacity(
            contact,
            cur_time,
            capmap,
        )
        if mbs_bytes:
            contact_mbs_bytes = min(contact_crcap_bytes, mbs_bytes)
        else:
            contact_mbs_bytes = contact_crcap_bytes
        if bundle_last_fragment_min_size_bytes == 0:
            # "Must not fragment" bit is set
            bundle_max_bytes: int = bundle_first_fragment_min_size_bytes
            bundle_header_size: int = 0
            bundle_min_size: int = bundle_first_fragment_min_size_bytes
        else:
            bundle_fragment_min_size_bytes = (
                bundle_first_fragment_min_size_bytes
                if not fragment_results
                else bundle_last_fragment_min_size_bytes
            )
            bundle_max_bytes = (
                bundle_fragment_min_size_bytes + frag_remaining_payload
            )
            bundle_header_size = bundle_fragment_min_size_bytes
            # At least the headers + one byte of the payload must be
            # transferred, except if the bundle must not be fragmented.
            bundle_min_size = bundle_fragment_min_size_bytes + 1
        logger.debug(
            "Considering contact: %s (cap = %d, bms = %d, mbs = %d, now = %d)",
            contact,
            contact_crcap_bytes,
            bundle_min_size,
            contact_mbs_bytes,
            _loop_time_to_contact_time(cur_time),
        )
        if contact_mbs_bytes < bundle_min_size:
            continue
        fragment_size: int = min(bundle_max_bytes, contact_mbs_bytes)
        fragment_payload_size = (
            fragment_size - bundle_header_size
        )
        logger.debug(
            "Assigning fragment of size = %d (pl_sz = %d)",
            fragment_size,
            fragment_payload_size,
        )
        if fragment_payload_size == bid.payload_length:
            # push the complete bundle
            fragment_results.append((
                contact,
                bid.fragment_offset,
                bid.payload_length,
                fragment_size,
            ))
        else:
            fragment_results.append((
                contact,
                fragment_offset,
                fragment_payload_size,
                fragment_size,
            ))
        fragment_offset += fragment_payload_size
        frag_remaining_payload -= fragment_payload_size
        if frag_remaining_payload <= 0:
            break
    if frag_remaining_payload > 0:
        raise ScheduleException("insufficient contact volume available")
    return set(fragment_results)


def _bundle_id(bdl: Bundle, bdl_disp_info: BundleDispatchInfo):
    return BundleIDType(
        src_eid=bdl.src_eid,
        creation_timestamp=bdl.creation_timestamp_ms,
        sequence_number=bdl.sequence_number,
        fragment_offset=bdl.fragment_offset,
        payload_length=bdl.payload_length,
        dst_eid=bdl.dst_eid,
        min_frag_size_first=bdl_disp_info.min_frag_size_first,
        min_frag_size_last=bdl_disp_info.min_frag_size_last,
    )


async def sleep_until(simtime: float) -> None:
    """Sleep using asyncio.sleep until the specified time is reached.

    This function returns immediately if the specified timestamp is in the
    past. This function may only be called from within asyncio callbacks,
    tasks, or coroutines.
    """
    duration = simtime - time.time()
    if duration > 0:
        await asyncio.sleep(duration)


def _build_storage_call(op: StorageOperation, bid: BundleIDType) -> bytes:
    cmd = StorageCall(
        operation=op,
        id=bid.to_proto(),
    )
    cmd_bytes = cmd.SerializeToString()
    return proto_encoder._VarintBytes(len(cmd_bytes)) + cmd_bytes


async def _send_storage_call(
    aap2_rpc_client: AAP2AsyncClient,
    storage_agent_eid: str,
    op: StorageOperation,
    bid: BundleIDType
):
    payload = _build_storage_call(
        op,
        bid,
    )
    assert (
        (await aap2_rpc_client.send_adu_rpc(
            BundleADU(
                dst_eid=storage_agent_eid,
                payload_length=len(payload),
                adu_flags=[BundleADUFlags.BUNDLE_ADU_WITH_BDM_AUTH],
            ),
            payload,
        )).response_status ==
        ResponseStatus.RESPONSE_STATUS_SUCCESS
    )


async def _push_bundle_from_storage(
    aap2_rpc_client: AAP2AsyncClient,
    storage_agent_eid: str,
    bid: BundleIDType
):
    await _send_storage_call(
        aap2_rpc_client,
        storage_agent_eid,
        StorageOperation.STORAGE_OPERATION_PUSH_BUNDLES,
        bid,
    )


async def _delete_bundle_from_storage(
    aap2_rpc_client: AAP2AsyncClient,
    storage_agent_eid: str,
    bid: BundleIDType
):
    await _send_storage_call(
        aap2_rpc_client,
        storage_agent_eid,
        StorageOperation.STORAGE_OPERATION_DELETE_BUNDLES,
        bid,
    )


async def _bundle_transmission(
    aap2_rpc_client: AAP2AsyncClient,
    storage_agent_eid: str,
    contact: ContactDefinition,
    queue: asyncio.Queue
):
    while True:
        try:
            bid = await queue.get()
        except (asyncio.IncompleteReadError, asyncio.CancelledError):
            return
        # NOTE: if the bundle is not in storage anymore, e.g., because it was
        # dropped or re-scheduled, this just silently fails. In the future we
        # may want to provide functionality to inform the BDM of that failure
        # (e.g., by a response sent by the storage agent).
        try:
            logger.debug(
                "Pushing bundle %s during contact %s",
                bid,
                contact,
            )
            await _push_bundle_from_storage(
                aap2_rpc_client,
                storage_agent_eid,
                bid,
            )
        except (asyncio.IncompleteReadError, asyncio.CancelledError):
            logger.debug(
                "Re-inserting bundle %s after termination of contact %s",
                bid,
                contact,
            )
            queue.put_nowait(bid)  # ensure it is re-scheduled
            return
        except Exception as e:
            logger.warning(
                "Bundle TX terminated with %s: %s",
                type(e).__name__,
                e,
            )
            raise


async def _reschedule_bundles(
    aap2_rpc_client: AAP2AsyncClient,
    storage_agent_eid: str,
    fwd_dict: Dict[str, Set[ContactDefinition]],
    capmap: Dict[ContactDefinition, Dict[BundleIDType, int]],
    scheduled_bundles: SchedDictType,
    sched_db: SchedDBType,
    contact: ContactDefinition,
):
    queue, sched_set, _ = sched_db[contact]
    # 1. get all bundle IDs from the scheduling queue for the contact
    # Note that every bundle may be scheduled just in part for that contact,
    # which means that parts (other fragments) are scheduled for other contacts
    # or were already sent.
    bid_set = set()
    try:
        while True:
            bid_set.add(queue.get_nowait())
    except asyncio.QueueEmpty:
        pass
    # 2. attempt to schedule each bid again
    for bid in bid_set:
        fo, fps, fs = sched_set[bid]
        try:
            fragment_results = _calc_bundle_schedule(
                fwd_dict,
                capmap,
                bid,
                0,  # no mbs as we already scheduled this bundle appropriately
                # from sched_set, as ID is for the whole/as-received bundle!
                fo,
                fps,
            )
            if len(fragment_results) == 0:
                raise ScheduleException("no route found")
        except ScheduleException as e:
            # any part failed -> drop it everywhere
            # drop from storage -> other parts may be pushed but do not arrive
            logger.info(
                "Could not re-schedule Bundle %s, deleting: %s",
                bid,
                e,
            )
            await _delete_bundle_from_storage(
                aap2_rpc_client,
                storage_agent_eid,
                bid,
            )
        else:
            logger.debug(
                "Bundle %s frag=(%d, %d) re-scheduled for: %s",
                bid,
                fo,
                fps,
                fragment_results,
            )
            for c, c_fo, c_fps, c_fs in fragment_results:
                c_queue, c_sched_set, _ = sched_db[c]
                c_queue.put_nowait(bid)
                c_sched_set[bid] = (c_fo, c_fps, c_fs)
            scheduled_bundles[bid].update(fragment_results)


async def _schedule_contact(
    aap2_rpc_client: AAP2AsyncClient,
    storage_agent_eid: str,
    active_contacts: Set[ContactDefinition],
    contact: ContactDefinition,
    contact_queue: asyncio.Queue,
    flsm: FIBLinkStatusManager,
    fwd_dict: Dict[str, Set[ContactDefinition]],
    capmap: Dict[ContactDefinition, Dict[BundleIDType, int]],
    scheduled_bundles: SchedDictType,
    sched_db: SchedDBType,
):
    start_time_loop = _contact_time_to_loop_time(contact.start)
    end_time_loop = _contact_time_to_loop_time(contact.end)
    logger.debug(
        "Contact: %s - sleeping until %d (%d s)",
        contact,
        start_time_loop,
        start_time_loop - time.time()
    )
    try:
        await sleep_until(start_time_loop)
        logger.debug("Contact started: %s", contact)
        active_contacts.add(contact)
        # initiate link via CLA
        assert (
            (await aap2_rpc_client.send_rpc(AAPMessage(link=Link(
                status=LinkStatus.LINK_STATUS_UP,
                peer_node_id=contact.next_hop_node_id,
                peer_cla_addr=contact.next_hop_cla_addr,
            )))).response_status ==
            ResponseStatus.RESPONSE_STATUS_SUCCESS
        )
        # Wait for link to become active if it is not already...
        await flsm.wait_until_active(contact.next_hop_cla_addr)
        try:
            await asyncio.wait_for(
                _bundle_transmission(
                    aap2_rpc_client,
                    storage_agent_eid,
                    contact,
                    contact_queue,
                ),
                timeout=(end_time_loop - time.time()),
            )
        except asyncio.TimeoutError:
            pass
        except Exception as e:
            logger.warning(
                "Bundle transmission failed for contact: %s; %s: %s",
                contact,
                type(e).__name__,
                e,
            )
            raise
        else:
            logger.debug(
                "Bundle transmission stopped for contact: %s",
                contact,
            )
            await sleep_until(end_time_loop)
    except asyncio.CancelledError:
        pass
    finally:
        logger.debug("Contact ended or terminated: %s", contact)
        await _reschedule_bundles(
            aap2_rpc_client,
            storage_agent_eid,
            fwd_dict,
            capmap,
            scheduled_bundles,
            sched_db,
            contact,
        )
        # in any case, attempt to tear down the CLA link
        assert (
            (await aap2_rpc_client.send_rpc(AAPMessage(link=Link(
                status=LinkStatus.LINK_STATUS_DOWN,
                peer_node_id=contact.next_hop_node_id,
                peer_cla_addr=contact.next_hop_cla_addr,
            )))).response_status ==
            ResponseStatus.RESPONSE_STATUS_SUCCESS
        )
        try:
            active_contacts.remove(contact)
            del sched_db[contact]
        except KeyError:
            pass


async def _dispatch_scheduled_bundle(
    scheduled_bundles: SchedDictType,
    sched_db: SchedDBType,
    active_contacts: Set[ContactDefinition],
    bid: BundleIDType
) -> Tuple[DispatchResult, FragmentDispatchSetType]:
    # Extract the set next hop + fragmentation settings from the
    # active contacts and then dispatch it there...
    fragment_dispatch_commands = set()
    for contact, _, _, _ in scheduled_bundles[bid]:
        if contact in active_contacts:
            try:
                queue, sched_set, task = sched_db[contact]
            except KeyError:
                continue
            fo, fps, fs = sched_set[bid]
            fragment_dispatch_commands.add(
                (contact, fo, fps, fs)
            )
    # If the length of the `fragment_dispatch_commands` set is 0,
    # the bundle should not be forwarded right now (or not anymore,
    # as rescheduling failed?).
    # Might be that the fuzzy EID glob called a wrong bundle
    # from storage. We explicitly drop it from the working
    # memory (forward to the empty set) as we can just call it
    # again later.
    disp_result = DispatchResult(
        next_hops=[
            DispatchResult.NextHopEntry(
                node_id=contact.next_hop_node_id,
                fragment_offset=fo,
                fragment_length=fs,
            )
            for contact, fo, fs, _ in fragment_dispatch_commands
        ],
    )
    logger.debug(
        "Scheduling entry found, dispatching result: %s",
        disp_result,
    )
    return disp_result, fragment_dispatch_commands


def _schedule_bundle(
    bid: BundleIDType,
    fwd_dict: Dict[str, Set[ContactDefinition]],
    capmap: Dict[ContactDefinition, Dict[BundleIDType, int]],
    scheduled_bundles: SchedDictType,
    sched_db: SchedDBType,
    storage_node_id: str,
    mbs_bytes: int,
) -> DispatchResult:
    try:
        fragment_results = _calc_bundle_schedule(
            fwd_dict,
            capmap,
            bid,
            mbs_bytes,
        )
        if len(fragment_results) == 0:
            raise ScheduleException("no route found")
    except ScheduleException as e:
        logger.debug(
            "Could not schedule bundle, issuing empty dispatch: %s",
            e,
        )
        # Drop bundle if we cannot schedule it (v0.13 behavior)
        # Dropping means generating the dispatch response worked
        # (SUCCESS), but we want to dispatch to the empty set.
        return DispatchResult(next_hops=[])
    except Exception as e:
        logger.warning("Scheduling failed: %s", e)
        raise
    else:
        scheduled_bundles[bid] = fragment_results
        for contact, _, _, fs in fragment_results:
            if contact not in capmap:
                capmap[contact] = {}
            capmap[contact][bid] = fs
        logger.debug(
            "Bundle %s scheduled for: %s",
            bid,
            fragment_results,
        )
        # Always dispatch to storage when scheduled successfully.
        return DispatchResult(
            next_hops=[DispatchResult.NextHopEntry(node_id=storage_node_id)],
        )


async def _dispatch_bundle(
    bid: BundleIDType,
    active_contacts: Set[ContactDefinition],
    scheduled_bundles: SchedDictType,
    sched_db: SchedDBType,
    stored_bundles: Set[BundleIDType],
    aap2_rpc_client: AAP2AsyncClient,
    storage_agent_eid: str,
    prevent_loops: bool,
) -> DispatchResult:
    if bid not in stored_bundles:
        logger.debug(
            "Bundle %s not stored (dropped on re-scheduling?), ignoring",
            bid,
        )
        return None
    # If no TX event, it is something we recalled from storage.
    dispatch_result, frag_dispatched = await _dispatch_scheduled_bundle(
        scheduled_bundles,
        sched_db,
        active_contacts,
        bid,
    )
    scheduled_bundles[bid] -= frag_dispatched
    # Delete the bundle from storage - a copy should be in the BP agent
    # connection still, until it has been transmitted finally.
    if len(scheduled_bundles[bid]) == 0:
        # NOTE: This emulates the v0.13.0 behavior - we always
        # drop a bundle after *attempting* to send it.
        logger.debug(
            "Bundle %s sent, deleting from storage.",
            str(bid),
        )
        await _delete_bundle_from_storage(
            aap2_rpc_client,
            storage_agent_eid,
            bid,
        )
        # NOTE if at some point we attempt re-dispatch, e.g., because of
        # failed TX, we must properly reference-count transmissions or
        # always keep bundle references here.
        if not prevent_loops:
            del scheduled_bundles[bid]
            stored_bundles.discard(bid)
    return dispatch_result


async def _process_dispatch_event(
    dr: DispatchEvent,
    fwd_dict: Dict[str, Set[ContactDefinition]],
    capmap: Dict[ContactDefinition, Dict[BundleIDType, int]],
    scheduled_bundles: SchedDictType,
    sched_db: SchedDBType,
    stored_bundles: Set[BundleIDType],
    active_contacts: Set[ContactDefinition],
    aap2_rpc_client: AAP2AsyncClient,
    storage_agent_eid: str,
    storage_node_id: str,
    prevent_loops: bool,
) -> DispatchResult:
    bdl = dr.bundle
    bid = _bundle_id(bdl, dr.additional_information)
    reason = dr.reason
    logger.debug(
        "Received dispatch request with reason %s for bundle %s to %s",
        str(reason),
        bid,
        bdl.dst_eid,
    )
    if bid in scheduled_bundles:
        if reason == DispatchReason.DISPATCH_REASON_TX_SUCCEEDED:
            if bid not in stored_bundles:
                # This was the initial "TX to storage"
                fragment_results = scheduled_bundles[bid]
                for contact, fo, fps, fs in fragment_results:
                    queue, sched_set, task = sched_db[contact]
                    queue.put_nowait(bid)
                    if bid in sched_set:
                        logger.info(
                            "Looping bundle recognized in active contact: %s",
                            bid,
                        )
                    sched_set[bid] = (fo, fps, fs)
                stored_bundles.add(bid)
        elif reason == DispatchReason.DISPATCH_REASON_TX_FAILED:
            # NOTE: Same behavior as v0.13.0 - this is the compat. BDM.
            logger.warning(
                "TX failed for bundle %s, DROPPING.",
                str(bid),
            )
        else:
            return await _dispatch_bundle(
                bid,
                active_contacts,
                scheduled_bundles,
                sched_db,
                stored_bundles,
                aap2_rpc_client,
                storage_agent_eid,
                prevent_loops,
            )
    elif reason not in VALID_SCHEDULE_DISPATCH_REASONS:
        logger.debug(
            "No scheduling entry found, but dispatch reason is "
            "%s, thus, ignoring it",
            str(reason),
        )
    else:
        return _schedule_bundle(
            bid,
            fwd_dict,
            capmap,
            scheduled_bundles,
            sched_db,
            storage_node_id,
            dr.additional_information.max_bundle_size_bytes,
        )
    return None


def _process_config(
    aap2_rpc_client: AAP2AsyncClient,
    adu: BundleADU,
    payload: bytes,
    contact_set: Set[ContactDefinition],
    fwd_dict: Dict[str, Set[ContactDefinition]],
    capmap: Dict[ContactDefinition, Dict[BundleIDType, int]],
    scheduled_bundles: SchedDictType,
    sched_db: SchedDBType,
    active_contacts: Set[ContactDefinition],
    flsm: FIBLinkStatusManager,
    storage_agent_eid: str,
    insecure_config: bool,
) -> Dict[str, Set[ContactDefinition]]:
    logger.debug(
        "Processing new configuration message from %s",
        adu.src_eid,
    )
    if (BundleADUFlags.BUNDLE_ADU_WITH_BDM_AUTH not in adu.adu_flags
            and not insecure_config):
        logger.warning(
            "Flag `BUNDLE_ADU_WITH_BDM_AUTH` not set for received config "
            "message and `--insecure-config` argument is not set, thus, "
            "dropping message as we cannot validate it is secure"
        )
        return fwd_dict
    add_set, rm_set = _decode_cfg_msg(
        contact_set,
        adu,
        payload,
    )
    contact_set.difference_update(rm_set)
    for contact in rm_set:
        logger.debug("Removing contact: %s", contact)
        try:
            queue, sched_set, task = sched_db[contact]
        except KeyError:
            continue
        task.cancel()
        # Will delete itself!
        # del sched_db[contact]
    contact_set.update(add_set)
    for contact in add_set:
        logger.debug("Adding contact: %s", contact)
        contact_queue: asyncio.Queue[BundleIDType] = asyncio.Queue()
        task = asyncio.create_task(_schedule_contact(
            aap2_rpc_client,
            storage_agent_eid,
            active_contacts,
            contact,
            contact_queue,
            flsm,
            # re-scheduling
            fwd_dict,
            capmap,
            scheduled_bundles,
            sched_db,
        ))
        assert contact not in sched_db
        sched_db[contact] = (contact_queue, {}, task)
    return _rebuild_fwd_dict(contact_set)


def _update_link_info(flsm: FIBLinkStatusManager, link: Link):
    next_hop = link.peer_cla_addr
    is_now_active = (link.status == LinkStatus.LINK_STATUS_UP)
    flsm.update_link_info(next_hop, is_now_active)
    logger.debug(
        "Updating link status for link with '%s' via '%s' to %d",
        link.peer_node_id,
        link.peer_cla_addr,
        is_now_active,
    )


async def run_compat_bdm(
    aap2_sub_client: AAP2AsyncClient,
    aap2_rpc_client: AAP2AsyncClient,
    storage_agent_eid: str,
    storage_node_id: str,
    prevent_loops: bool,
    insecure_config: bool,
):
    contact_set: Set[ContactDefinition] = set()
    active_contacts: Set[ContactDefinition] = set()
    # scheduled_bundles: Dict[BundleIDType, Set[ContactDefinition]] = {}
    scheduled_bundles: SchedDictType = {}
    capmap: Dict[ContactDefinition, Dict[BundleIDType, int]] = {}
    stored_bundles: Set[BundleIDType] = set()
    fwd_dict: Dict[str, Set[ContactDefinition]] = {}
    sched_db: SchedDBType = {}
    flsm = FIBLinkStatusManager()
    logger.info("Waiting for dispatch requests...")

    while True:
        try:
            msg = await aap2_sub_client.receive_msg()
        except AAP2ServerDisconnected:
            logger.warning("µD3TN has closed the connection.")
            sys.exit(1)
        except asyncio.exceptions.CancelledError:
            logger.info("Terminated by keyboard interrupt.")
            sys.exit(130)  # exit status for SIGINT
        response_status = ResponseStatus.RESPONSE_STATUS_SUCCESS
        dispatch_result = None
        if msg.WhichOneof("msg") == "dispatch_event":
            dispatch_result = await _process_dispatch_event(
                msg.dispatch_event,
                fwd_dict,
                capmap,
                scheduled_bundles,
                sched_db,
                stored_bundles,
                active_contacts,
                aap2_rpc_client,
                storage_agent_eid,
                storage_node_id,
                prevent_loops,
            )
        elif msg.WhichOneof("msg") == "adu":
            _, payload = await aap2_sub_client.receive_adu(msg.adu)
            fwd_dict_new = _process_config(
                aap2_rpc_client,
                msg.adu,
                payload,
                contact_set,
                fwd_dict,
                capmap,
                scheduled_bundles,
                sched_db,
                active_contacts,
                flsm,
                storage_agent_eid,
                insecure_config,
            )
            # Update in-place as contact tasks may hold a reference!
            fwd_dict.clear()
            fwd_dict.update(fwd_dict_new)
        elif msg.WhichOneof("msg") == "link":
            _update_link_info(flsm, msg.link)
        elif msg.WhichOneof("msg") == "keepalive":
            response_status = ResponseStatus.RESPONSE_STATUS_ACK
        else:
            logger.debug("Received message with field '%s' set, discarding.",
                         msg.WhichOneof("msg"))
        await aap2_sub_client.send(AAPResponse(
            response_status=response_status,
            dispatch_result=dispatch_result,
        ))


async def execute_bdm(
    aap2_sub_client: AAP2AsyncClient,
    aap2_rpc_client: AAP2AsyncClient,
    agentid: str,
    secret: str,
    storage_agent_eid: Optional[str],
    storage_node_id: str,
    prevent_loops: bool,
    insecure_config: bool,
    keepalive_seconds: int,
):
    async with aap2_sub_client, aap2_rpc_client:
        is_ipn = aap2_sub_client.is_ipn_eid
        default_config_agent_id = (
            DEFAULT_CONFIG_AGENT_ID_IPN
            if is_ipn
            else DEFAULT_CONFIG_AGENT_ID_DTN
        )
        secret = await aap2_sub_client.configure(
            agentid or default_config_agent_id,
            subscribe=True,
            secret=secret,
            auth_type=AuthType.AUTH_TYPE_FIB_AND_DISPATCH,
            keepalive_seconds=keepalive_seconds,
        )
        await aap2_rpc_client.configure(
            agentid or default_config_agent_id,
            subscribe=False,
            secret=secret,
            auth_type=AuthType.AUTH_TYPE_FIB_AND_DISPATCH,
        )
        if storage_agent_eid is None:
            if aap2_rpc_client.is_ipn_eid:
                storage_agent_eid = f"{aap2_rpc_client.eid_prefix}.9003"
            else:
                storage_agent_eid = f"{aap2_rpc_client.eid_prefix}sqlite"
        logger.info("AAP 2.0 agents registered and configured!")
        logger.info("Assigned agent secret: '%s'", secret)
        await run_compat_bdm(
            aap2_sub_client,
            aap2_rpc_client,
            storage_agent_eid,
            storage_node_id,
            prevent_loops,
            insecure_config,
        )


def main():
    parser = argparse.ArgumentParser(
        description=(
            "bundle dispatcher module realizing the uD3TN v0.13 forwarding"
            "approach via the AAP 2.0 BDM interface"
        ),
    )
    add_common_parser_arguments(parser)
    add_keepalive_parser_argument(parser)
    parser.add_argument(
        "--storage-agent-eid",
        default=None,
        help="the (full) EID of the bundle storage agent (default: <auto>)",
    )
    parser.add_argument(
        "--storage-node-id",
        default="dtn:storage",
        help="the next-hop node ID of the bundle storage system",
    )
    parser.add_argument(
        "--prevent-loops",
        action="store_true",
        help="keep bundle references, so we find and drop looping bundles",
    )
    parser.add_argument(
        "--insecure-config",
        action="store_true",
        help=(
            "do not check that the sender of a config bundle is local and has "
            "authenticated with the administrative / BDM credential"
        ),
    )

    args = parser.parse_args()
    global logger
    logger = initialize_logger(args.verbosity)

    if args.tcp:
        aap2_sub_client = AAP2AsyncTCPClient(address=args.tcp)
        aap2_rpc_client = AAP2AsyncTCPClient(address=args.tcp)
    else:
        aap2_sub_client = AAP2AsyncUnixClient(address=args.socket)
        aap2_rpc_client = AAP2AsyncUnixClient(address=args.socket)
    asyncio.run(execute_bdm(
        aap2_sub_client,
        aap2_rpc_client,
        args.agentid,
        get_secret_from_args(args),
        args.storage_agent_eid,
        args.storage_node_id,
        args.prevent_loops,
        args.insecure_config,
        args.keepalive_seconds,
    ))


if __name__ == "__main__":
    main()


def test_parse_config():
    assert _parse_config(
        "1(dtn://ud3tn2.dtn/):(mtcp:127.0.0.1:4223)::["
        "{1401519306972,1401519316972,1200,[(dtn://89326/),(dtn://12349/)]},"
        "{1401519506972,1401519516972,1200,[(dtn://89326/),(dtn://12349/)]}];"
    ) == (
        1,
        "dtn://ud3tn2.dtn/",
        {
            ContactDefinition(
                start=1401519306972,
                end=1401519316972,
                data_rate=1200,
                next_hop_node_id="dtn://ud3tn2.dtn/",
                next_hop_cla_addr="mtcp:127.0.0.1:4223",
                reachable_eids={
                    "dtn://89326/",
                    "dtn://12349/",
                },
            ),
            ContactDefinition(
                start=1401519506972,
                end=1401519516972,
                data_rate=1200,
                next_hop_node_id="dtn://ud3tn2.dtn/",
                next_hop_cla_addr="mtcp:127.0.0.1:4223",
                reachable_eids={
                    "dtn://89326/",
                    "dtn://12349/",
                },
            ),
        },
    )
    assert _parse_config(
        "1(dtn://ud3tn2.dtn/)::[(dtn://18471/),(dtn://81491/)]:"
        "[{1401519406972,1401819306972,1200}];"
    ) == (
        1,
        "dtn://ud3tn2.dtn/",
        {
            ContactDefinition(
                start=1401519406972,
                end=1401819306972,
                data_rate=1200,
                next_hop_node_id="dtn://ud3tn2.dtn/",
                next_hop_cla_addr=None,
                reachable_eids=[
                    "dtn://18471/",
                    "dtn://81491/",
                ],
            ),
        },
    )
    assert _parse_config(
        "2(dtn://ud3tn2.dtn/):(mtcp:127.0.0.1:4223):"
        "[(dtn://89326/),(dtn://12349/)];"
    ) == (
        2,
        "dtn://ud3tn2.dtn/",
        set(),
    )
    assert _parse_config(
        "2(dtn://ud3tn2.dtn/)::[]:[];"
    ) == (
        2,
        "dtn://ud3tn2.dtn/",
        set(),
    )
    assert _parse_config(
        "2(ipn:1.0)::[];"
    ) == (
        2,
        "ipn:1.0",
        set(),
    )
    assert _parse_config(
        "3(dtn://ud3tn2.dtn/);"
    ) == (
        3,
        "dtn://ud3tn2.dtn/",
        set(),
    )
    assert _parse_config(
        "1(dtn://13714/):(tcpspp:):[(dtn://18471/),(dtn://81491/)]:;"
    ) == (
        1,
        "dtn://13714/",
        set(),
    )
