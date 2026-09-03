# -*- coding: utf-8 -*-
"""BSController-internal storage invariants."""

from __future__ import annotations

from typing import Iterator

from ..model import BS_CONTROLLER, ClusterState, Finding, critical, error
from ..registry import check
from ..views import bsc_view
from ._util import capped

# TGroupId::Zero(): the column default, meaning "no group assigned".
UNASSIGNED_GROUP = 0


@check(
    id="I7",
    title="BSController slots reference existing groups and disks",
    needs={BS_CONTROLLER: ["Group", "VSlot", "PDisk"]},
    tags=("storage", "bscontroller"),
)
def vslots_reference_existing_groups(state: ClusterState) -> Iterator[Finding]:
    """Broken by a stale BSController: slots survive on disks while the group
    row that explains them is gone, or a slot id gets handed out twice on the
    same PDisk."""
    bsc = bsc_view(state)
    groups = bsc.group_ids()
    pdisks = {p.ref: p for p in bsc.pdisks()}

    dangling = []
    orphan_disk = []
    max_slot_per_pdisk = {}

    for slot in bsc.vslots():
        if slot.group_id not in (None, UNASSIGNED_GROUP) and slot.group_id not in groups:
            dangling.append(slot)

        pdisk_ref = "%s:%s" % (slot.node_id, slot.pdisk_id)
        if pdisk_ref not in pdisks:
            orphan_disk.append(slot)
        elif slot.vslot_id is not None:
            current = max_slot_per_pdisk.get(pdisk_ref)
            if current is None or slot.vslot_id > current:
                max_slot_per_pdisk[pdisk_ref] = slot.vslot_id

    yield from capped(
        dangling,
        lambda slot: critical(
            "vslot %s references group %s, missing in BSController" % (slot.ref, slot.group_id),
            vslot=slot.ref,
            group_id=slot.group_id,
        ),
        lambda total, rest: critical(
            "%d vslots reference groups missing in BSController" % total,
            total=total,
            sample=[s.ref for s in rest[:100]],
        ),
    )

    yield from capped(
        orphan_disk,
        lambda slot: error(
            "vslot %s sits on a PDisk that BSController does not know" % slot.ref,
            vslot=slot.ref,
        ),
        lambda total, rest: error(
            "%d vslots sit on unknown PDisks" % total,
            total=total,
            sample=[s.ref for s in rest[:100]],
        ),
    )

    for ref, max_slot in sorted(max_slot_per_pdisk.items()):
        pdisk = pdisks[ref]
        if pdisk.next_vslot_id is not None and pdisk.next_vslot_id <= max_slot:
            yield critical(
                "PDisk %s NextVSlotId %d is not above its max used VSlotID %d, "
                "the slot would be handed out twice" % (ref, pdisk.next_vslot_id, max_slot),
                pdisk=ref,
                next_vslot_id=pdisk.next_vslot_id,
                max_vslot_id=max_slot,
            )
