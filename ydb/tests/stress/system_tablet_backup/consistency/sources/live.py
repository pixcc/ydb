# -*- coding: utf-8 -*-
"""Reader for the running cluster.

Tenant SchemeShards and tenant Hives are never backed up -- ``ITablet::NeedBackup``
returns false as soon as ``TenantPathId`` is set.  They are, however, exactly the
tablets a cluster-tablet restore does *not* roll back, so they keep holding the
references the restored tablet has forgotten.  The only way to see them is to
ask the live cluster.

Uses ``urllib`` rather than ``requests`` to keep the package importable with a
bare Python 3 on a production host.
"""

from __future__ import annotations

import json
import ssl
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, Iterable, Mapping, Optional, Set, Tuple

from ..model import LiveCluster, LiveHive, LivePath

DEFAULT_TIMEOUT_SECONDS = 30


class LiveError(Exception):
    pass


def _get_json(base: str, path: str, params: Dict[str, Any], timeout: int, insecure: bool) -> Any:
    url = "%s%s?%s" % (base.rstrip("/"), path, urllib.parse.urlencode(params))
    context = None
    if insecure and url.startswith("https"):
        context = ssl._create_unverified_context()
    try:
        with urllib.request.urlopen(url, timeout=timeout, context=context) as response:
            return json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError) as exc:
        raise LiveError("%s: %s" % (url, exc))


def read_hive(
    mon_endpoint: str,
    hive_id: int,
    timeout: int = DEFAULT_TIMEOUT_SECONDS,
    insecure: bool = False,
) -> LiveHive:
    """Ask one Hive for the tablets it owns.

    ``/viewer/json/hiveinfo`` proxies TEvRequestHiveInfo to the given hive, so it
    works for tenant Hives just as well as for the root one.  With ``ui64=false``
    the viewer renders 64-bit ids as strings, which is why every id is parsed
    through ``int()``.
    """
    try:
        body = _get_json(
            mon_endpoint,
            "/viewer/json/hiveinfo",
            {"hive_id": hive_id, "ui64": "false", "timeout": timeout * 1000},
            timeout,
            insecure,
        )
    except LiveError as exc:
        return LiveHive(hive_id=hive_id, reachable=False, error=str(exc))

    tablet_ids: Set[int] = set()
    owners: Set[Tuple[int, int]] = set()

    for tablet in body.get("Tablets", []) or []:
        raw_id = tablet.get("TabletID")
        if raw_id is None:
            continue
        tablet_ids.add(int(raw_id))
        owner = tablet.get("TabletOwner") or {}
        owner_id, owner_idx = owner.get("Owner"), owner.get("OwnerIdx")
        if owner_id is not None and owner_idx is not None:
            owners.add((int(owner_id), int(owner_idx)))

    return LiveHive(hive_id=hive_id, tablet_ids=tablet_ids, owners=owners)


# Where the version lives in a describe result, per object kind.  The two
# protocols are the same but the field names are not
# (ydb/core/protos/flat_scheme_op.proto: TBlockStoreVolumeDescription.AlterVersion,
# TFileStoreDescription.Version).
_VERSION_FIELDS = {
    "blockstore": ("BlockStoreVolumeDescription", "AlterVersion"),
    "filestore": ("FileStoreDescription", "Version"),
}

# Read the path for its identity only: no version, any object will do.
KIND_ANY = "path"


def _as_int(value: Any) -> Optional[int]:
    # ui64=false renders 64-bit numbers as strings, so both forms show up.
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def read_path(
    mon_endpoint: str,
    path: str,
    kind: str,
    timeout: int = DEFAULT_TIMEOUT_SECONDS,
    insecure: bool = False,
) -> LivePath:
    """Read one live path: its identity, and its version when it has one.

    ``/viewer/json/describe`` returns TEvDescribeSchemeResult as JSON, so the
    version is read out of the same field SchemeShard would send to the tablet,
    and the identity out of the same TDirEntry a describe would return to a
    client.  A path that is simply gone is not an error here -- it is answered
    as ``exists=False`` and judged by the caller.
    """
    holder, field = _VERSION_FIELDS.get(kind, (None, None))
    if holder is None and kind != KIND_ANY:
        return LivePath(path=path, kind=kind, reachable=False, error="unknown kind %r" % kind)

    try:
        body = _get_json(
            mon_endpoint,
            "/viewer/json/describe",
            {"path": path, "ui64": "false", "timeout": timeout * 1000},
            timeout,
            insecure,
        )
    except LiveError as exc:
        return LivePath(path=path, kind=kind, reachable=False, error=str(exc))

    description = body.get("PathDescription") or {}
    node = description.get(holder) if holder is not None else description
    if not node:
        status = str(body.get("Status") or "")
        reason = str(body.get("Reason") or "")
        # No description of the expected kind: either the path is gone, or it
        # is there but is something else entirely.  Both are the caller's call.
        return LivePath(
            path=path,
            kind=kind,
            exists=False,
            error=(" ".join(x for x in (status, reason) if x)).strip(),
        )

    version = None
    if holder is not None:
        raw = node.get(field)
        # Protobuf JSON drops zero-valued scalars, so a missing field means 0
        # here rather than "unknown": the description itself is present.
        version = int(raw) if raw is not None else 0

    # CreateFinished plus the creating TxId is what proves that a particular
    # operation -- one a stale backup may still hold in flight -- has already
    # run to the end in the live cluster.
    entry = description.get("Self") or {}
    return LivePath(
        path=path,
        kind=kind,
        version=version,
        owner_id=_as_int(entry.get("SchemeshardId")),
        path_id=_as_int(entry.get("PathId")),
        create_tx_id=_as_int(entry.get("CreateTxId")),
        create_finished=bool(entry.get("CreateFinished")),
    )


def read_live(
    mon_endpoint: str,
    hive_ids: Iterable[int] = (),
    paths: Optional[Mapping[str, str]] = None,
    timeout: int = DEFAULT_TIMEOUT_SECONDS,
    insecure: bool = False,
) -> LiveCluster:
    """Read every named Hive and every named versioned path.

    ``paths`` maps a full path to its kind, as ``discover_versioned_paths``
    returns it.  Unreachable hives and paths are recorded rather than raised: on
    a cluster mid-recovery some of them may legitimately be down, and the checks
    report that as an explicit gap in coverage instead of silently passing.
    """
    cluster = LiveCluster(source=mon_endpoint)
    for hive_id in sorted(set(hive_ids)):
        cluster.hives[hive_id] = read_hive(mon_endpoint, hive_id, timeout, insecure)
    for path, kind in sorted((paths or {}).items()):
        cluster.paths[path] = read_path(mon_endpoint, path, kind, timeout, insecure)
    return cluster


def discover_tenant_hives(state: Any) -> Dict[int, str]:
    """Tenant Hive ids the root SchemeShard backup knows about.

    Returns ``{hive tablet id: database name}``.  A database without its own
    Hive uses a shared one, which is included too -- a shared Hive is still a
    tenant Hive as far as backups are concerned.
    """
    from ..views import schemeshard_views

    found: Dict[int, str] = {}
    for ss in schemeshard_views(state):
        for subdomain in ss.databases():
            hive_id = subdomain.effective_hive_id
            if hive_id:
                found[hive_id] = subdomain.name
    return found


def discover_operation_paths(state: Any) -> Dict[str, str]:
    """Paths targeted by operations the SchemeShard backup still holds in flight.

    Returns ``{full path: KIND_ANY}``, mergeable with ``discover_versioned_paths``.
    Reading these is how doctor learns that an operation the restored tablet
    would resume has in fact already finished, which is the only thing that
    makes dropping it safe.
    """
    from ..views import schemeshard_views

    found: Dict[str, str] = {}
    for ss in schemeshard_views(state):
        names = ss.path_names()
        for tx in ss.txs_in_flight():
            if tx.target_path_id is None:
                continue
            name = names.get(tx.target_path_id)
            if name:
                found[name] = KIND_ANY
    return found


def discover_versioned_paths(state: Any) -> Dict[str, str]:
    """Versioned objects the SchemeShard backup knows about.

    Returns ``{full path: kind}``.  Dropped paths are left out: nothing live is
    left to compare them against.
    """
    from ..views import schemeshard_views

    found: Dict[str, str] = {}
    for ss in schemeshard_views(state):
        dropped = {p.path_id for p in ss.paths() if p.is_dropped}
        names = ss.path_names()
        for obj in ss.versioned_objects():
            if obj.path_id in dropped:
                continue
            name = names.get(obj.path_id)
            if name:
                found[name] = obj.kind
    return found
