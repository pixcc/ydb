# -*- coding: utf-8 -*-
"""Builder for synthetic system tablet backups.

Produces exactly the layout ``flat_executor_backup.cpp`` writes, so the reader
is exercised against the real format rather than a convenient approximation.
"""

from __future__ import annotations

import hashlib
import json
import os
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence

# Column type ids are irrelevant to the reader (values arrive already encoded),
# so a single placeholder keeps the fixtures readable.
_ANY_TYPE = 4


class TableBuilder:
    def __init__(self, table_id: int, name: str, key_columns: Sequence[str]):
        self.table_id = table_id
        self.name = name
        self.key_columns = list(key_columns)
        self.columns: List[str] = []
        self.rows: List[Dict[str, Any]] = []

    def add_rows(self, rows: Iterable[Mapping[str, Any]]) -> "TableBuilder":
        for row in rows:
            for column in row:
                if column not in self.columns:
                    self.columns.append(column)
            self.rows.append(dict(row))
        return self

    def schema_delta(self) -> List[Dict[str, Any]]:
        columns = list(self.columns)
        for key in self.key_columns:
            if key not in columns:
                columns.append(key)

        delta: List[Dict[str, Any]] = [
            {"delta_type": "AddTable", "table_id": self.table_id, "table_name": self.name}
        ]
        column_ids = {name: index for index, name in enumerate(columns)}
        for name, column_id in column_ids.items():
            delta.append(
                {
                    "delta_type": "AddColumn",
                    "table_id": self.table_id,
                    "column_id": column_id,
                    "column_name": name,
                    "column_type": _ANY_TYPE,
                }
            )
        for key in self.key_columns:
            delta.append(
                {
                    "delta_type": "AddColumnToKey",
                    "table_id": self.table_id,
                    "column_id": column_ids[key],
                }
            )
        return delta


class BackupBuilder:
    """One tablet's backup: tables, plus optional changelog commits."""

    def __init__(self, tablet_type: str, tablet_id: int, generation: int = 1, step: int = 1,
                 timestamp: str = "20260818120000Z"):
        self.tablet_type = tablet_type
        self.tablet_id = tablet_id
        self.generation = generation
        self.step = step
        self.timestamp = timestamp
        self.tables: Dict[str, TableBuilder] = {}
        self.commits: List[Dict[str, Any]] = []
        self._next_table_id = 1

    def table(self, name: str, key_columns: Sequence[str]) -> TableBuilder:
        if name not in self.tables:
            self.tables[name] = TableBuilder(self._next_table_id, name, key_columns)
            self._next_table_id += 1
        return self.tables[name]

    def commit(self, step: int, changes: Sequence[Mapping[str, Any]]) -> "BackupBuilder":
        """Append a changelog commit; each change is {table, op, ...columns}."""
        self.commits.append({"step": step, "data_changes": [dict(c) for c in changes]})
        return self

    @property
    def dir_name(self) -> str:
        return "backup_%s_g%d_s%d" % (self.timestamp, self.generation, self.step)

    def write(self, root: str) -> str:
        backup_dir = os.path.join(root, self.tablet_type, str(self.tablet_id), self.dir_name)
        snapshot_dir = os.path.join(backup_dir, "snapshot")
        os.makedirs(snapshot_dir)

        delta: List[Dict[str, Any]] = []
        for table in self.tables.values():
            delta.extend(table.schema_delta())
        files = [_write_json(snapshot_dir, "schema.json", {"delta": delta, "rewrite": False})]

        for table in self.tables.values():
            files.append(_write_ndjson(snapshot_dir, "%s.json" % table.name, table.rows))

        manifest = {
            "tablet_type": self.tablet_type,
            "tablet_id": self.tablet_id,
            "generation": self.generation,
            "step": self.step,
            "files": files,
        }
        manifest_str = json.dumps(manifest)
        with open(os.path.join(snapshot_dir, "manifest.json"), "w") as handle:
            handle.write(manifest_str)
        with open(os.path.join(snapshot_dir, "manifest.json.sha256"), "w") as handle:
            handle.write(hashlib.sha256(manifest_str.encode()).hexdigest())

        if self.commits:
            # Real changelog lines carry prev_sha256: the running sha256 of
            # everything written before them, each line with its newline.  The
            # restore rejects a line whose value does not match, so fixtures
            # have to build the chain too.
            body = b""
            for commit in self.commits:
                chained = dict(commit)
                chained["prev_sha256"] = hashlib.sha256(body).hexdigest()
                body += (json.dumps(chained) + "\n").encode()
            with open(os.path.join(backup_dir, "changelog.json"), "wb") as handle:
                handle.write(body)
            with open(os.path.join(backup_dir, "changelog.json.sha256"), "w") as handle:
                handle.write(hashlib.sha256(body).hexdigest())

        return backup_dir


def _write_json(directory: str, name: str, payload: Any) -> Dict[str, str]:
    body = json.dumps(payload)
    with open(os.path.join(directory, name), "w") as handle:
        handle.write(body)
    return {"name": name, "sha256": hashlib.sha256(body.encode()).hexdigest()}


def _write_ndjson(directory: str, name: str, rows: Sequence[Mapping[str, Any]]) -> Dict[str, str]:
    body = "".join(json.dumps(row) + "\n" for row in rows)
    with open(os.path.join(directory, name), "w") as handle:
        handle.write(body)
    return {"name": name, "sha256": hashlib.sha256(body.encode()).hexdigest()}


# --------------------------------------------------------------------------
# A small but realistic cluster: one SchemeShard object with two shards, each
# tablet bound to one storage group.
# --------------------------------------------------------------------------

SS_TABLET_ID = 72057594046678944
HIVE_TABLET_ID = 72057594037968897
BSC_TABLET_ID = 72057594037932033

# Hive's allocator counts the low 44 bits, so uniq(72075186224037888) is
# 0x10000 -- the first id the root Hive ever hands out.  The fixtures keep the
# two spaces apart on purpose: confusing them is exactly the bug this suite is
# meant to catch.
UNIQ_PART_MASK = 0x00000FFFFFFFFFFF


def uniq(tablet_id):
    return tablet_id & UNIQ_PART_MASK


SHARDS = [
    {"ShardIdx": 1, "TabletId": 72075186224037888, "PathId": 2, "TabletType": 2},
    {"ShardIdx": 2, "TabletId": 72075186224037889, "PathId": 3, "TabletType": 2},
]
GROUPS = [2181038080, 2181038081]

# Tenant tablets: a database whose SchemeShard, Hive, coordinator and mediator
# are shards of the root SchemeShard but have no backups of their own.
TENANT_PATH_ID = 10
TENANT_NAME = "db1"
TENANT_SS_TABLET_ID = 72075186224037900
TENANT_HIVE_TABLET_ID = 72075186224037901
TENANT_SHARDS = [
    {"ShardIdx": 10, "TabletId": TENANT_SS_TABLET_ID, "PathId": TENANT_PATH_ID, "TabletType": 16},
    {"ShardIdx": 11, "TabletId": TENANT_HIVE_TABLET_ID, "PathId": TENANT_PATH_ID, "TabletType": 14},
    {"ShardIdx": 12, "TabletId": 72075186224037902, "PathId": TENANT_PATH_ID, "TabletType": 13},
    {"ShardIdx": 13, "TabletId": 72075186224037903, "PathId": TENANT_PATH_ID, "TabletType": 5},
]


def hive_backup(
    tablets: Optional[Sequence[Mapping[str, Any]]] = None,
    # A uniq part, not a composed tablet id -- see the note above.
    next_tablet_id: int = 0x10002,
    channel_groups: Optional[Sequence[Mapping[str, Any]]] = None,
    sequences: Optional[Sequence[Mapping[str, Any]]] = None,
    **kwargs: Any,
) -> BackupBuilder:
    builder = BackupBuilder("hive", HIVE_TABLET_ID, **kwargs)

    if tablets is None:
        tablets = [
            {
                "ID": shard["TabletId"],
                "Owner": [SS_TABLET_ID, shard["ShardIdx"]],
                "State": 200,  # ReadyToWork
                "TabletType": shard["TabletType"],
                "KnownGeneration": 1,
            }
            for shard in SHARDS
        ]
    builder.table("Tablet", ["ID"]).add_rows(tablets)

    if channel_groups is None:
        channel_groups = [
            {
                "Tablet": shard["TabletId"],
                "Channel": 0,
                "Generation": 1,
                "Group": GROUPS[index],
                "DeletedAtGeneration": 0,
            }
            for index, shard in enumerate(SHARDS)
        ]
    builder.table("TabletChannelGen", ["Tablet", "Channel", "Generation"]).add_rows(channel_groups)

    # TSchemeIds::State::NextTabletId == 0
    builder.table("State", ["Key"]).add_rows([{"Key": 0, "Value": next_tablet_id}])
    builder.table("Sequences", ["OwnerId", "OwnerIdx"]).add_rows(sequences or [])
    return builder


def schemeshard_backup(
    shards: Optional[Sequence[Mapping[str, Any]]] = None,
    next_path_id: int = 4,
    next_shard_idx: int = 3,
    paths: Optional[Sequence[Mapping[str, Any]]] = None,
    subdomains: Optional[Sequence[Mapping[str, Any]]] = None,
    subdomain_shards: Optional[Sequence[Mapping[str, Any]]] = None,
    txs_in_flight: Optional[Sequence[Mapping[str, Any]]] = None,
    tx_shards: Optional[Sequence[Mapping[str, Any]]] = None,
    volumes: Optional[Sequence[Mapping[str, Any]]] = None,
    volume_alters: Optional[Sequence[Mapping[str, Any]]] = None,
    file_stores: Optional[Sequence[Mapping[str, Any]]] = None,
    file_store_alters: Optional[Sequence[Mapping[str, Any]]] = None,
    **kwargs: Any,
) -> BackupBuilder:
    builder = BackupBuilder("scheme_shard", SS_TABLET_ID, **kwargs)

    builder.table("Shards", ["ShardIdx"]).add_rows(SHARDS if shards is None else shards)
    builder.table("MigratedShards", ["OwnerShardId", "LocalShardId"]).add_rows([])
    builder.table("SubDomains", ["PathId"]).add_rows(subdomains or [])
    builder.table("SubDomainShards", ["PathId", "ShardIdx"]).add_rows(subdomain_shards or [])

    if paths is None:
        paths = [
            {"Id": 1, "ParentId": 0, "Name": "Root", "PathType": 1, "StepDropped": 0},
            {"Id": 2, "ParentId": 1, "Name": "t_1", "PathType": 2, "StepDropped": 0},
            {"Id": 3, "ParentId": 1, "Name": "t_2", "PathType": 2, "StepDropped": 0},
        ]
    builder.table("Paths", ["Id"]).add_rows(paths)

    # An operation that has finished erases its own rows, so a clean backup has
    # none: everything left here is what the restored SchemeShard resumes.
    builder.table("TxInFlightV2", ["TxId", "TxPartId"]).add_rows(txs_in_flight or [])
    builder.table("TxShardsV2", ["TxId", "TxPartId", "ShardIdx"]).add_rows(tx_shards or [])

    builder.table("BlockStoreVolumes", ["PathId"]).add_rows(volumes or [])
    builder.table("BlockStoreVolumeAlters", ["PathId"]).add_rows(volume_alters or [])
    builder.table("FileStoreInfos", ["PathId"]).add_rows(file_stores or [])
    builder.table("FileStoreAlters", ["PathId"]).add_rows(file_store_alters or [])

    # SysParams values are Utf8, so they arrive as decimal strings.
    builder.table("SysParams", ["Id"]).add_rows(
        [{"Id": 1, "Value": str(next_path_id)}, {"Id": 2, "Value": str(next_shard_idx)}]
    )
    return builder


def bsc_backup(
    groups: Optional[Sequence[int]] = None,
    next_group_id: Optional[int] = None,
    vslots: Optional[Sequence[Mapping[str, Any]]] = None,
    pdisks: Optional[Sequence[Mapping[str, Any]]] = None,
    **kwargs: Any,
) -> BackupBuilder:
    builder = BackupBuilder("bscontroller", BSC_TABLET_ID, **kwargs)

    group_ids = list(GROUPS if groups is None else groups)
    builder.table("Group", ["ID"]).add_rows([{"ID": g, "Generation": 1} for g in group_ids])

    if next_group_id is None:
        next_group_id = (max(group_ids) + 1) if group_ids else 1
    builder.table("State", ["FixedKey"]).add_rows(
        [{"FixedKey": True, "NextGroupID": next_group_id}]
    )

    if pdisks is None:
        pdisks = [{"NodeID": 1, "PDiskID": 1, "Guid": 111, "NextVSlotId": len(group_ids) + 1}]
    builder.table("PDisk", ["NodeID", "PDiskID"]).add_rows(pdisks)

    if vslots is None:
        vslots = [
            {
                "NodeID": 1,
                "PDiskID": 1,
                "VSlotID": index + 1,
                "GroupID": group_id,
                "GroupGeneration": 1,
            }
            for index, group_id in enumerate(group_ids)
        ]
    builder.table("VSlot", ["NodeID", "PDiskID", "VSlotID"]).add_rows(vslots)
    return builder
