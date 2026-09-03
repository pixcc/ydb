# -*- coding: utf-8 -*-
"""Checks that need the workload ledger.

Only runnable on a test stand.  On production they are reported as skipped,
which is the point of the requirement mechanism: the same command works in both
places and tells you honestly what it could not verify.

Expected ledger records::

    {"op": "create", "status": "ok", "path": "/Root/t_41", "path_id": 41,
     "shards": [{"shard_idx": 77, "tablet_id": 72075186224037965}]}
    {"op": "drop", "status": "ok", "path": "/Root/t_41"}
"""

from __future__ import annotations

import time
from typing import Dict, Iterator, Optional, Set

from ..model import (
    HIVE,
    LEDGER,
    SCHEME_SHARD,
    ClusterState,
    Finding,
    critical,
    error,
    info,
    warning,
)
from ..registry import check
from ..views import hive_view, schemeshard_views, uniq_part
from ._util import capped
from .meta import FRESHNESS_WARN_SECONDS
from .sequences import _reissue_predicate


def _dropped_paths(state: ClusterState) -> Set[str]:
    return {
        entry.get("path")
        for entry in state.ledger.of_op("drop")
        if entry.get("status") == "ok" and entry.get("path")
    }


@check(
    id="I8",
    title="Everything the ledger created still exists in SchemeShard",
    needs={LEDGER: [], SCHEME_SHARD: ["Paths", "Shards", "MigratedShards"]},
    tags=("ledger", "schemeshard"),
)
def created_objects_survive(state: ClusterState) -> Iterator[Finding]:
    """The end-to-end statement: an object the workload created, and never
    dropped, must still be resolvable after the restore."""
    dropped = _dropped_paths(state)

    live_path_ids: Set[int] = set()
    for ss in schemeshard_views(state):
        for path in ss.paths():
            if not path.is_dropped:
                live_path_ids.add(path.path_id)

    lost = []
    unverifiable = 0

    for entry in state.ledger.of_op("create"):
        if entry.get("status") != "ok":
            continue
        path = entry.get("path")
        if path in dropped:
            continue
        path_id = entry.get("path_id")
        if path_id is None:
            unverifiable += 1
            continue
        if path_id not in live_path_ids:
            lost.append((path, path_id))

    yield from capped(
        lost,
        lambda item: error(
            "object %s (path id %s) was created successfully but SchemeShard has no live path"
            % (item[0], item[1]),
            path=item[0],
            path_id=item[1],
        ),
        lambda total, rest: error(
            "%d objects created by the workload are missing from SchemeShard" % total,
            total=total,
            sample=[i[0] for i in rest[:100]],
        ),
    )

    if unverifiable:
        yield info(
            "%d ledger create records carry no path id and were not checked" % unverifiable,
            count=unverifiable,
        )


@check(
    id="I12",
    title="Allocators are ahead of every id the ledger ever observed",
    needs={LEDGER: [], HIVE: ["Tablet", "State", "Sequences"]},
    tags=("ledger", "sequences", "corruption"),
)
def allocators_ahead_of_ledger(state: ClusterState) -> Iterator[Finding]:
    """The only check that still works when Hive *and* SchemeShard were both
    rolled back: the ledger is then the sole witness that an id was ever used."""
    hive = hive_view(state)

    observed: Dict[int, str] = {}
    for entry in state.ledger.of_op("create"):
        if entry.get("status") != "ok":
            continue
        for shard in entry.get("shards", []) or []:
            tablet_id = shard.get("tablet_id")
            if tablet_id:
                observed[int(tablet_id)] = entry.get("path", "")

    if not observed:
        yield info("ledger recorded no tablet ids, nothing to check")
        return

    known = {t.tablet_id for t in hive.tablets()}
    stored_next = hive.next_tablet_id()
    own_max = max((uniq_part(t) for t in known), default=0)
    effective_next = max(stored_next or 0, (own_max + 1) if known else 0)
    # The allocator counts unique parts, not composed tablet ids.
    will_reissue = _reissue_predicate(hive.sequences(), effective_next)

    at_risk = sorted(t for t in observed if will_reissue(uniq_part(t)))

    yield from capped(
        at_risk,
        lambda tablet_id: critical(
            "tablet id %d (uniq part %d) was handed out to %s but Hive will allocate it again"
            % (tablet_id, uniq_part(tablet_id), observed[tablet_id] or "<unknown path>"),
            tablet_id=tablet_id,
            uniq_part=uniq_part(tablet_id),
            path=observed[tablet_id],
            known_to_hive=tablet_id in known,
            effective_next_tablet_id=effective_next,
        ),
        lambda total, rest: critical(
            "%d tablet ids from the ledger will be handed out again by Hive" % total,
            total=total,
            effective_next_tablet_id=effective_next,
            sample=rest[:100],
        ),
    )


@check(
    id="I14",
    title="Tablet states dated against the ledger rather than file timestamps",
    needs={LEDGER: [], HIVE: ["Tablet"], SCHEME_SHARD: ["Paths"]},
    tags=("ledger", "meta"),
)
def ledger_dated_staleness(state: ClusterState) -> Iterator[Finding]:
    """The precise staleness measure, immune to how the backups were copied.

    ``I11`` has to fall back on the mtime of ``changelog.json``, because the
    backup format carries no wall clock: the directory name records when the
    snapshot *started*, and commits carry a tablet step.  Here the ledger dates
    each tablet's state directly -- by the newest operation whose effect the
    tablet still shows.
    """
    creates = [e for e in state.ledger.of_op("create") if e.get("status") == "ok"]
    if not creates:
        yield info("ledger recorded no successful creates, nothing to date against")
        return

    hive_tablets = {t.tablet_id for t in hive_view(state).tablets()}
    ss_path_ids = {p.path_id for ss in schemeshard_views(state) for p in ss.paths()}

    def newest_reflected(predicate) -> Optional[float]:
        stamps = [e.ts for e in creates if predicate(e)]
        return max(stamps) if stamps else None

    seen = {
        HIVE: newest_reflected(
            lambda e: any(
                shard.get("tablet_id") in hive_tablets for shard in e.get("shards", []) or []
            )
        ),
        SCHEME_SHARD: newest_reflected(lambda e: e.get("path_id") in ss_path_ids),
    }
    dated = {name: ts for name, ts in seen.items() if ts is not None}
    if len(dated) < 2:
        yield info(
            "only %d tablet state(s) could be dated against the ledger" % len(dated),
            dated=sorted(dated),
        )
        return

    newest = max(dated.values())
    lags = {name: newest - ts for name, ts in dated.items()}
    worst = max(lags.values())

    detail = {
        name: {
            "lag_seconds": round(lag, 3),
            "newest_ledger_op_reflected": time.strftime(
                "%Y-%m-%d %H:%M:%SZ", time.gmtime(dated[name])
            ),
        }
        for name, lag in lags.items()
    }
    message = "ledger-dated staleness: %s" % ", ".join(
        "%s -%.1fs" % (name, lag) for name, lag in sorted(lags.items(), key=lambda kv: -kv[1])
    )

    if worst >= FRESHNESS_WARN_SECONDS:
        yield warning(message, **detail)
    else:
        yield info(message, **detail)
