# -*- coding: utf-8 -*-
"""What a restored SchemeShard does next.

The other modules ask whether the restored state is self-consistent.  This one
asks what the restored tablet *does* when it starts working again: an operation
that was still in flight when the backup was taken is resumed on boot and
re-sent to shards that finished it long ago, and a rolled-back version is
re-sent to a tablet that refuses to go backwards.  Both are crash loops rather
than lost references, so they belong in their own module.
"""

from __future__ import annotations

from typing import Dict, Iterator, List, Optional, Tuple

from ..model import (
    SCHEME_SHARD,
    ClusterState,
    Finding,
    critical,
    error,
    info,
    warning,
)
from ..registry import check
from ..views import ETxState, VersionedObject, schemeshard_views
from ._util import capped


def _tablets(shard_idxs: Tuple[int, ...], by_idx: Dict[int, Optional[int]]) -> List[int]:
    return [t for t in (by_idx.get(idx) for idx in shard_idxs) if t]


def _shards_text(shard_idxs: Tuple[int, ...], by_idx: Dict[int, Optional[int]]) -> str:
    if not shard_idxs:
        return "no shards"
    parts = []
    for idx in shard_idxs:
        tablet_id = by_idx.get(idx)
        parts.append("%d (tablet %d)" % (idx, tablet_id) if tablet_id else "%d" % idx)
    return "shards " + ", ".join(parts)


@check(
    id="I19",
    title="A restored SchemeShard replays nothing at the live shards",
    needs={
        SCHEME_SHARD: [
            "TxInFlightV2",
            "TxShardsV2",
            "Shards",
            "MigratedShards",
            "Paths",
            "BlockStoreVolumes",
            "BlockStoreVolumeAlters",
            "FileStoreInfos",
            "FileStoreAlters",
        ],
    },
    tags=("replay", "schemeshard"),
)
def restored_schemeshard_replays_nothing(state: ClusterState) -> Iterator[Finding]:
    """Broken by a stale SchemeShard: it resumes operations the shards have
    already completed, and re-sends configuration versions the volumes have
    already moved past.  Both end in a restart loop, not in a failed request."""

    surviving = []
    finished = []
    other = []

    for ss in schemeshard_views(state):
        names = ss.path_names()
        tablet_by_idx: Dict[int, Optional[int]] = {
            shard.shard_idx: shard.tablet_id
            for shard in ss.shards()
            if shard.owner_tablet_id == ss.tablet_id
        }

        for tx in ss.txs_in_flight():
            path = names.get(tx.target_path_id, "<path %s>" % tx.target_path_id)
            item = (ss, tx, path, tablet_by_idx)
            if tx.talks_to_shards:
                surviving.append(item)
            elif tx.finished:
                finished.append(item)
            else:
                other.append(item)

    def _details(item) -> Dict[str, object]:
        ss, tx, path, by_idx = item
        return dict(
            schemeshard=ss.tablet_id,
            tx=tx.ref,
            tx_type=tx.type_name,
            state=tx.state_name,
            path=path,
            path_id=tx.target_path_id,
            shard_idxs=list(tx.shard_idxs),
            tablet_ids=_tablets(tx.shard_idxs, by_idx),
        )

    def _resumed_message(item) -> str:
        _, tx, path, by_idx = item
        message = (
            "operation %s (%s) survived the replay at %s on %s and will be resumed against %s"
            % (tx.ref, tx.type_name, tx.state_name, path, _shards_text(tx.shard_idxs, by_idx))
        )
        # BlockStore and FileStore go one step further than a stuck operation:
        # the tablet answers ERROR_BAD_VERSION to an outdated config, which is a
        # status SchemeShard asserts on rather than handles, and the assertion
        # takes down the whole node -- so the restart loop is the control plane's.
        if tx.versioned_config and tx.state == ETxState.CONFIGURE_PARTS:
            message += (
                "; if the tablet has already applied a newer config it answers "
                "ERROR_BAD_VERSION, which aborts the SchemeShard node"
            )
        return message

    yield from capped(
        surviving,
        lambda item: critical(_resumed_message(item), **_details(item)),
        lambda total, rest: critical(
            "%d schema operations survived the replay in a state that talks to shards" % total,
            total=total,
            sample=[item[1].ref for item in rest[:100]],
        ),
    )

    yield from capped(
        finished,
        lambda item: warning(
            "operation %s (%s) is %s but its row was not erased in the backup"
            % (item[1].ref, item[1].type_name, item[1].state_name),
            **_details(item)
        ),
        lambda total, rest: warning(
            "%d finished operations still have a TxInFlightV2 row" % total,
            total=total,
            sample=[item[1].ref for item in rest[:100]],
        ),
    )

    yield from capped(
        other,
        lambda item: error(
            "operation %s (%s) survived the replay at %s on %s"
            % (item[1].ref, item[1].type_name, item[1].state_name, item[2]),
            **_details(item)
        ),
        lambda total, rest: error(
            "%d schema operations survived the replay" % total,
            total=total,
            sample=[item[1].ref for item in rest[:100]],
        ),
    )

    yield from _version_rollbacks(state)


# The version SchemeShard holds is authoritative only as long as it is not older
# than the tablet's.  Below that line, the tablet rejects everything SchemeShard
# sends, and SchemeShard treats the rejection as impossible:
#
#   Y_VERIFY_S(status == OK || status == ERROR_UPDATE_IN_PROGRESS, ...)
#       schemeshard__operation_common_bsv.cpp:33  (create and alter volume)
#       schemeshard__operation_create_fs.cpp:54, schemeshard__operation_alter_fs.cpp:53
#
# Y_VERIFY_S panics the process, so every retry of the operation kills the node
# again.  Equality is safe: the tablet answers OK to a config it has already
# applied, which is what makes a replay of the *current* version harmless.
def _version_rollbacks(state: ClusterState) -> Iterator[Finding]:
    rolled_back = []
    ahead = []
    gone = []
    unreachable = []
    verified = 0
    unverified = 0

    live_paths = state.live.paths if state.live is not None else {}

    for ss in schemeshard_views(state):
        names = ss.path_names()
        dropped = {p.path_id for p in ss.paths() if p.is_dropped}

        for obj in ss.versioned_objects():
            if obj.path_id in dropped:
                continue
            path = names.get(obj.path_id, "<path %d>" % obj.path_id)
            live = live_paths.get(path)
            if live is None:
                unverified += 1
                continue
            if not live.reachable:
                unreachable.append((path, obj, live))
                continue
            if not live.exists:
                gone.append((path, obj, live))
                continue

            verified += 1
            backup_version = obj.pending_version
            if backup_version is None or live.version is None:
                continue
            if backup_version < live.version:
                rolled_back.append((path, obj, live))
            elif backup_version > live.version:
                ahead.append((path, obj, live))

    def _details(path: str, obj: VersionedObject, live) -> Dict[str, object]:
        return dict(
            path=path,
            path_id=obj.path_id,
            kind=obj.kind,
            backup_version=obj.version,
            backup_pending_version=obj.alter_version,
            live_version=live.version,
        )

    yield from capped(
        rolled_back,
        lambda t: critical(
            "restore rolls %s back from version %s to %s: the %s tablet has already applied "
            "the newer config and answers ERROR_BAD_VERSION, which aborts the SchemeShard node"
            % (t[0], t[2].version, t[1].pending_version, t[1].kind),
            **_details(*t)
        ),
        lambda total, rest: critical(
            "%d versioned objects are rolled back below the version the tablet has applied"
            % total,
            total=total,
            sample=[t[0] for t in rest[:100]],
        ),
    )

    yield from capped(
        ahead,
        lambda t: warning(
            "%s is at version %s in the backup but at %s live: the object was probably "
            "recreated, so the restored SchemeShard describes a %s that no longer exists"
            % (t[0], t[1].pending_version, t[2].version, t[1].kind),
            **_details(*t)
        ),
        lambda total, rest: warning(
            "%d versioned objects are ahead of the live cluster" % total,
            total=total,
            sample=[t[0] for t in rest[:100]],
        ),
    )

    yield from capped(
        gone,
        lambda t: warning(
            "%s is a %s in the backup but the live cluster has no such object%s"
            % (t[0], t[1].kind, (": " + t[2].error) if t[2].error else ""),
            **_details(*t)
        ),
        lambda total, rest: warning(
            "%d versioned objects of the backup are absent from the live cluster" % total,
            total=total,
            sample=[t[0] for t in rest[:100]],
        ),
    )

    yield from capped(
        unreachable,
        lambda t: warning(
            "could not read the live version of %s: %s" % (t[0], t[2].error),
            **_details(*t)
        ),
        lambda total, rest: warning(
            "%d versioned objects could not be read from the live cluster" % total,
            total=total,
            sample=[t[0] for t in rest[:100]],
        ),
    )

    if verified:
        yield info(
            "%d versioned object(s) checked against the live cluster" % verified,
            count=verified,
        )

    if unverified:
        yield warning(
            "%d versioned object(s) were not verified against a live tablet: a version older "
            "than the tablet's aborts the SchemeShard node, pass --mon-endpoint to check them"
            % unverified,
            count=unverified,
        )
