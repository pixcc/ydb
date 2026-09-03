# -*- coding: utf-8 -*-
"""Typed views over raw table dumps.

This is the only module that knows the local database schemas of the system
tablets, so a column rename upstream is a one-file change here.  Sources:

    Hive          ydb/core/mind/hive/hive_schema.h
    SchemeShard   ydb/core/tx/schemeshard/schemeshard_schema.h
    BSController  ydb/core/mind/bscontroller/scheme.h
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, Iterator, List, Optional, Tuple

from .model import TabletDump, as_int, as_pair

# --------------------------------------------------------------------------
# Hive
# --------------------------------------------------------------------------

# NHive::TSchemeIds::State -- NextTabletId is the first enumerator, hence 0.
HIVE_STATE_NEXT_TABLET_ID = 0

# Tablet ids are composed (ydb/core/base/tabletid.h):
#
#     MakeTabletID(fromHive, uniqPart) = (1 << 56) | (fromHive << 44) | uniqPart
#
# Hive's allocator -- State[NextTabletId], Sequences and TabletOwners -- works in
# the *unique part* space, while Tablet.ID and every reference from other tablets
# carry the full id.  Comparing the two directly is meaningless, so everything
# that touches the allocator goes through uniq_part().
TABLET_ID_UNIQ_PART_MASK = 0x00000FFFFFFFFFFF

# Ids in this window are never handed out (AvoidReservedUniqPartsBySystemTablets).
TABLET_ID_BLACKHOLE_BEGIN = 0x800000
TABLET_ID_BLACKHOLE_END = 0x900000


def uniq_part(tablet_id: int) -> int:
    """The part of a tablet id that Hive's allocator actually counts."""
    return tablet_id & TABLET_ID_UNIQ_PART_MASK


class ETabletState:
    """NHive::ETabletState (hive.h)."""

    UNKNOWN = 0
    GROUP_ASSIGNMENT = 50
    STOPPING_IN_GROUP_ASSIGNMENT = 98
    STOPPING = 99
    STOPPED = 100
    READY_TO_WORK = 200
    BLOCK_STORAGE = 201
    DELETING = 202

    NAMES = {
        0: "Unknown",
        50: "GroupAssignment",
        98: "StoppingInGroupAssignment",
        99: "Stopping",
        100: "Stopped",
        200: "ReadyToWork",
        201: "BlockStorage",
        202: "Deleting",
    }

    @classmethod
    def name(cls, value: Optional[int]) -> str:
        return cls.NAMES.get(value, "state=%s" % value)


@dataclass(frozen=True)
class HiveTablet:
    tablet_id: int
    owner: Optional[Tuple[int, int]]
    state: Optional[int]
    tablet_type: Optional[int]
    known_generation: Optional[int]

    @property
    def owner_tablet_id(self) -> Optional[int]:
        return self.owner[0] if self.owner else None

    @property
    def owner_idx(self) -> Optional[int]:
        return self.owner[1] if self.owner else None

    @property
    def is_deleting(self) -> bool:
        return self.state == ETabletState.DELETING


@dataclass(frozen=True)
class HiveChannelGen:
    tablet_id: int
    channel: Optional[int]
    generation: Optional[int]
    group: Optional[int]
    deleted_at_generation: int

    @property
    def is_history(self) -> bool:
        """True for a superseded channel generation."""
        return self.deleted_at_generation != 0


@dataclass(frozen=True)
class HiveSequence:
    """A row of Hive's Sequences table.

    The table holds two different things, and confusing them produces alarming
    nonsense on a healthy cluster:

    * ``OwnerId == 0`` (``TSequencer::NO_OWNER``) -- a range this Hive allocates
      from.  ``Next`` is its live allocation point.
    * ``OwnerId != 0`` -- a record of a range *granted away* to that Hive
      (mirrored in ``TabletOwners``).  ``Next`` is a snapshot from grant time and
      means nothing here; the range is reserved, not available.

    ``End`` is exclusive: the root's initial sequence ends at
    ``TABLET_ID_BLACKHOLE_BEGIN``, which is itself never handed out.
    """

    owner_id: Optional[int]
    owner_idx: Optional[int]
    begin: Optional[int]
    end: Optional[int]
    next: Optional[int]

    @property
    def is_allocation_source(self) -> bool:
        return self.owner_id in (0, None)

    @property
    def is_grant(self) -> bool:
        return not self.is_allocation_source


@dataclass(frozen=True)
class HiveGrant:
    """A tablet id range delegated to another Hive (Hive's TabletOwners)."""

    begin: int
    end: int
    owner_id: int


class HiveView:
    TABLE_TABLET = "Tablet"
    TABLE_CHANNEL_GEN = "TabletChannelGen"
    TABLE_STATE = "State"
    TABLE_SEQUENCES = "Sequences"
    TABLE_TABLET_OWNERS = "TabletOwners"

    def __init__(self, dump: TabletDump):
        self.dump = dump

    @property
    def tablet_id(self) -> int:
        return self.dump.tablet_id

    def tablets(self) -> Iterator[HiveTablet]:
        for row in self.dump.rows(self.TABLE_TABLET):
            tablet_id = as_int(row.get("ID"))
            if tablet_id is None:
                continue
            yield HiveTablet(
                tablet_id=tablet_id,
                owner=as_pair(row.get("Owner")),
                state=as_int(row.get("State")),
                tablet_type=as_int(row.get("TabletType")),
                known_generation=as_int(row.get("KnownGeneration")),
            )

    def tablets_by_id(self) -> Dict[int, HiveTablet]:
        return {t.tablet_id: t for t in self.tablets()}

    def channel_generations(self) -> Iterator[HiveChannelGen]:
        for row in self.dump.rows(self.TABLE_CHANNEL_GEN):
            tablet_id = as_int(row.get("Tablet"))
            if tablet_id is None:
                continue
            yield HiveChannelGen(
                tablet_id=tablet_id,
                channel=as_int(row.get("Channel")),
                generation=as_int(row.get("Generation")),
                group=as_int(row.get("Group")),
                deleted_at_generation=as_int(row.get("DeletedAtGeneration"), 0) or 0,
            )

    def state_value(self, key: int) -> Optional[int]:
        for row in self.dump.rows(self.TABLE_STATE):
            if as_int(row.get("Key")) == key:
                return as_int(row.get("Value"))
        return None

    def next_tablet_id(self) -> Optional[int]:
        return self.state_value(HIVE_STATE_NEXT_TABLET_ID)

    def sequences(self) -> List[HiveSequence]:
        return [
            HiveSequence(
                owner_id=as_int(row.get("OwnerId")),
                owner_idx=as_int(row.get("OwnerIdx")),
                begin=as_int(row.get("Begin")),
                end=as_int(row.get("End")),
                next=as_int(row.get("Next")),
            )
            for row in self.dump.rows(self.TABLE_SEQUENCES)
        ]

    def allocation_sources(self) -> List[HiveSequence]:
        """Only the ranges this Hive hands out from."""
        return [s for s in self.sequences() if s.is_allocation_source]

    def grants(self) -> List[HiveGrant]:
        """Ranges delegated to other Hives, from TabletOwners.

        These are the ids tenant Hives mint from.  Nothing else in a backup
        records them, and the tenant side has no backup of its own.
        """
        result: List[HiveGrant] = []
        for row in self.dump.rows(self.TABLE_TABLET_OWNERS):
            begin, end = as_int(row.get("Begin")), as_int(row.get("End"))
            owner = as_int(row.get("OwnerId"))
            if begin is None or end is None or owner is None:
                continue
            if owner == self.tablet_id:
                continue  # a range this Hive kept for itself
            result.append(HiveGrant(begin=begin, end=end, owner_id=owner))
        return result


# --------------------------------------------------------------------------
# SchemeShard
# --------------------------------------------------------------------------

# TSchemeShard::SysParam_* (schemeshard_schema.h)
SS_SYS_PARAM_NEXT_PATH_ID = 1
SS_SYS_PARAM_NEXT_SHARD_IDX = 2

# schemeshard InvalidTabletId: "absent" is stored as this, not as NULL or zero.
INVALID_TABLET_ID = 0xFFFFFFFFFFFFFFFF


class ETabletType:
    """NKikimrTabletBase::TTabletTypes::EType (ydb/core/protos/tablet.proto)."""

    MEDIATOR = 5
    COORDINATOR = 13
    HIVE = 14
    BS_CONTROLLER = 15
    SCHEME_SHARD = 16
    TX_ALLOCATOR = 23

    NAMES = {
        5: "Mediator",
        13: "Coordinator",
        14: "Hive",
        15: "BSController",
        16: "SchemeShard",
        23: "TxAllocator",
    }

    @classmethod
    def name(cls, value: Optional[int]) -> str:
        return cls.NAMES.get(value, "type=%s" % value)


@dataclass(frozen=True)
class Shard:
    """A shard owned by this SchemeShard, from Shards or MigratedShards."""

    owner_tablet_id: int
    shard_idx: int
    tablet_id: Optional[int]
    path_id: Optional[int]
    tablet_type: Optional[int]
    migrated: bool = False

    @property
    def ref(self) -> str:
        return "%d:%d" % (self.owner_tablet_id, self.shard_idx)


@dataclass(frozen=True)
class Subdomain:
    """A database, as the *root* SchemeShard sees it.

    The tablets listed here -- the tenant SchemeShard, tenant Hive, coordinators
    and mediators -- have no backups of their own: ``ITablet::NeedBackup``
    returns false as soon as ``TenantPathId`` is set.  They are therefore the
    side that is never rolled back, and every reference between them and a
    restored cluster tablet has to be judged against the live cluster.
    """

    path_id: int
    name: str
    shared_hive_id: Optional[int]
    shards: Tuple[Shard, ...] = ()
    # The domain the SchemeShard itself serves is listed in SubDomains too, but
    # it is not a tenant database: it has no tenant SchemeShard and no shard
    # rows of its own.
    is_root_domain: bool = False

    def _of_type(self, tablet_type: int) -> Tuple[int, ...]:
        return tuple(s.tablet_id for s in self.shards if s.tablet_type == tablet_type and s.tablet_id)

    @property
    def scheme_shard_id(self) -> Optional[int]:
        found = self._of_type(ETabletType.SCHEME_SHARD)
        return found[0] if found else None

    @property
    def hive_id(self) -> Optional[int]:
        """The database's own Hive, if it has one rather than a shared one."""
        found = self._of_type(ETabletType.HIVE)
        return found[0] if found else None

    @property
    def effective_hive_id(self) -> Optional[int]:
        return self.hive_id or self.shared_hive_id

    @property
    def coordinators(self) -> Tuple[int, ...]:
        return self._of_type(ETabletType.COORDINATOR)

    @property
    def mediators(self) -> Tuple[int, ...]:
        return self._of_type(ETabletType.MEDIATOR)

    @property
    def tablet_ids(self) -> Tuple[int, ...]:
        return tuple(s.tablet_id for s in self.shards if s.tablet_id)


@dataclass(frozen=True)
class Path:
    path_id: int
    parent_id: Optional[int]
    name: str
    path_type: Optional[int]
    step_dropped: int

    @property
    def is_dropped(self) -> bool:
        return self.step_dropped != 0


# NKikimr::NSchemeShard::ETxType (schemeshard_subop_types.h).  The enum is dense
# from zero, so the names are kept as a tuple indexed by value.  The header
# carries a "DO NOT REORDER" warning, which is what makes this safe.
TX_TYPE_NAMES = (
    "TxInvalid", "TxMkDir", "TxCreateTable", "TxCreatePQGroup", "TxAlterPQGroup",
    "TxAlterTable", "TxDropTable", "TxDropPQGroup", "TxModifyACL", "TxRmDir", "TxCopyTable",
    "TxSplitTablePartition", "TxBackup", "TxCreateSubDomain", "TxDropSubDomain",
    "TxCreateRtmrVolume", "TxCreateBlockStoreVolume", "TxAlterBlockStoreVolume",
    "TxAssignBlockStoreVolume", "TxDropBlockStoreVolume", "TxCreateKesus", "TxDropKesus",
    "TxForceDropSubDomain", "TxCreateSolomonVolume", "TxDropSolomonVolume", "TxAlterKesus",
    "TxAlterSubDomain", "TxAlterUserAttributes", "TxCreateTableIndex", "TxDropTableIndex",
    "TxCreateExtSubDomain", "TxMergeTablePartition", "TxAlterExtSubDomain",
    "TxForceDropExtSubDomain", "TxFillIndex", "TxUpgradeSubDomain",
    "TxUpgradeSubDomainDecision", "TxInitializeBuildIndex", "TxCreateLock",
    "TxAlterTableIndex", "TxFinalizeBuildIndex", "TxAlterSolomonVolume", "TxDropLock",
    "TxDropTableIndexAtMainTable", "TxCreateFileStore", "TxAlterFileStore",
    "TxDropFileStore", "TxRestore", "TxCreateOlapStore", "TxAlterOlapStore",
    "TxDropOlapStore", "TxCreateColumnTable", "TxAlterColumnTable", "TxDropColumnTable",
    "TxCreateCdcStream", "TxCreateCdcStreamAtTable", "TxAlterCdcStream",
    "TxAlterCdcStreamAtTable", "TxDropCdcStream", "TxDropCdcStreamAtTable", "TxMoveTable",
    "TxMoveTableIndex", "TxCreateSequence", "TxAlterSequence", "TxDropSequence",
    "TxCreateReplication", "TxAlterReplication", "TxDropReplicationCascade",
    "TxCreateBlobDepot", "TxAlterBlobDepot", "TxDropBlobDepot",
    "TxUpdateMainTableOnIndexMove", "TxAllocatePQ",
    "TxCreateCdcStreamAtTableWithInitialScan", "TxAlterExtSubDomainCreateHive",
    "TxAlterCdcStreamAtTableDropSnapshot", "TxDropCdcStreamAtTableDropSnapshot",
    "TxCreateExternalTable", "TxDropExternalTable", "TxAlterExternalTable",
    "TxCreateExternalDataSource", "TxDropExternalDataSource", "TxAlterExternalDataSource",
    "TxCreateView", "TxAlterView", "TxDropView", "TxCopySequence", "TxDropReplication",
    "TxCreateContinuousBackup", "TxAlterContinuousBackup", "TxDropContinuousBackup",
    "TxCreateResourcePool", "TxDropResourcePool", "TxAlterResourcePool",
    "TxRestoreIncrementalBackupAtTable", "TxCreateBackupCollection",
    "TxDropBackupCollection", "TxAlterBackupCollection", "TxMoveSequence",
    "TxCreateTransfer", "TxAlterTransfer", "TxDropTransfer", "TxDropTransferCascade",
    "TxCreateSysView", "TxDropSysView", "TxCreateLongIncrementalRestoreOp",
    "TxChangePathState", "TxRotateCdcStream", "TxRotateCdcStreamAtTable",
    "TxIncrementalRestoreFinalize", "TxCreateLongIncrementalBackupOp", "TxCreateSecret",
    "TxAlterSecret", "TxDropSecret", "TxCreateStreamingQuery", "TxDropStreamingQuery",
    "TxAlterStreamingQuery", "TxTruncateTable", "TxReadOnlyCopyColumnTable",
    "TxPrepareIndexValidation", "TxCreateFullBackupOp", "TxCreateLocalIndex",
    "TxDropLocalIndex", "TxAlterLocalIndex", "TxMoveLocalIndex", "TxCreateTestShardSet",
    "TxDropTestShardSet",
)


def tx_type_name(value: Optional[int]) -> str:
    if value is not None and 0 <= value < len(TX_TYPE_NAMES):
        return TX_TYPE_NAMES[value]
    return "TxType=%s" % value


class ETxState:
    """NKikimr::NSchemeShard::ETxState (schemeshard_subop_state_types.h)."""

    INVALID = 0
    WAITING = 1
    CREATE_PARTS = 2
    CONFIGURE_PARTS = 3
    DROP_PARTS = 4
    DELETE_PARTS = 5
    PROPOSE = 128
    PROPOSED_WAIT_PARTS = 129
    DONE = 240
    ABORTED = 250

    NAMES = {
        0: "Invalid", 1: "Waiting", 2: "CreateParts", 3: "ConfigureParts",
        4: "DropParts", 5: "DeleteParts", 6: "PublishTenantReadOnly", 7: "PublishGlobal",
        8: "RewriteOwners", 9: "PublishTenant", 10: "DoneMigrateTree", 11: "DeleteTenantSS",
        128: "Propose", 129: "ProposedWaitParts", 130: "ProposedDeleteParts",
        131: "TransferData", 132: "NotifyPartitioningChanged", 133: "Aborting",
        134: "DeleteExternalShards", 135: "DeletePrivateShards",
        136: "WaitShadowPathPublication", 137: "DeletePathBarrier", 138: "SyncHive",
        139: "CopyTableBarrier", 140: "ProposedCopySequence", 141: "ProposedMoveSequence",
        240: "Done", 250: "Aborted",
    }

    # States whose ProgressState sends a message to the shards.  These are the
    # ones a restored SchemeShard replays against tablets that have already
    # moved on -- see the operation state machines in
    # schemeshard__operation_*.cpp.
    TALKS_TO_SHARDS = frozenset({CREATE_PARTS, CONFIGURE_PARTS, DROP_PARTS, DELETE_PARTS})

    # The operation is over; the row is erased at the end of the transaction
    # that finishes it, so seeing one means the erase never reached the backup.
    FINISHED = frozenset({DONE, ABORTED})

    @classmethod
    def name(cls, value: Optional[int]) -> str:
        return cls.NAMES.get(value, "state=%s" % value)


# Operations whose ConfigureParts sends a versioned config to a live tablet that
# refuses anything older than what it already applied.  The refusal comes back
# as a status SchemeShard does not tolerate:
#
#   Y_VERIFY_S(status == OK || status == ERROR_UPDATE_IN_PROGRESS, ...)
#       ydb/core/tx/schemeshard/schemeshard__operation_common_bsv.cpp:33
#       ydb/core/tx/schemeshard/schemeshard__operation_create_fs.cpp:54
#       ydb/core/tx/schemeshard/schemeshard__operation_alter_fs.cpp:53
#
# and Y_VERIFY_S aborts the whole node process, not just the tablet.
VERSIONED_CONFIG_TX_TYPES = {
    16: "TxCreateBlockStoreVolume",
    17: "TxAlterBlockStoreVolume",
    44: "TxCreateFileStore",
    45: "TxAlterFileStore",
}


@dataclass(frozen=True)
class TxInFlight:
    """A row of TxInFlightV2: a schema operation SchemeShard had not finished.

    A completed operation erases its row, so every row that survives the
    snapshot plus changelog replay is an operation the *restored* SchemeShard
    will pick up and drive again.
    """

    tx_id: int
    part_id: int
    tx_type: Optional[int]
    state: Optional[int]
    target_path_id: Optional[int]
    min_step: Optional[int]
    plan_step: Optional[int]
    # Shard indices this sub-operation addresses, from TxShardsV2.
    shard_idxs: Tuple[int, ...] = ()

    @property
    def ref(self) -> str:
        return "%d:%d" % (self.tx_id, self.part_id)

    @property
    def type_name(self) -> str:
        return tx_type_name(self.tx_type)

    @property
    def state_name(self) -> str:
        return ETxState.name(self.state)

    @property
    def talks_to_shards(self) -> bool:
        return self.state in ETxState.TALKS_TO_SHARDS

    @property
    def finished(self) -> bool:
        return self.state in ETxState.FINISHED

    @property
    def versioned_config(self) -> Optional[str]:
        """Name of the operation when it re-sends a versioned config, else None."""
        return VERSIONED_CONFIG_TX_TYPES.get(self.tx_type)


@dataclass(frozen=True)
class VersionedObject:
    """A BlockStore volume or a FileStore, with the version SchemeShard holds.

    ``kind`` is what the viewer calls it, so the same value selects the field to
    read out of a live describe.
    """

    path_id: int
    kind: str            # "blockstore" | "filestore"
    version: Optional[int]
    # An alter row means an alter was in flight when the snapshot was taken;
    # its version is the one ConfigureParts would send.  The row can exist with
    # no version column of its own, so its presence is tracked separately.
    alter_version: Optional[int] = None
    has_alter: bool = False

    @property
    def pending_version(self) -> Optional[int]:
        return self.alter_version if self.alter_version is not None else self.version


class SchemeShardView:
    TABLE_SHARDS = "Shards"
    TABLE_MIGRATED_SHARDS = "MigratedShards"
    TABLE_PATHS = "Paths"
    TABLE_SYS_PARAMS = "SysParams"
    TABLE_SUB_DOMAINS = "SubDomains"
    TABLE_SUB_DOMAIN_SHARDS = "SubDomainShards"
    TABLE_TX_IN_FLIGHT = "TxInFlightV2"
    TABLE_TX_SHARDS = "TxShardsV2"
    # Pre-V2 spellings of the same rows.  LoadTxShards reads all three into one
    # list, so anything that removes an operation has to cover all three too.
    TABLE_TX_SHARDS_V1 = "TxShards"
    TABLE_MIGRATED_TX_SHARDS = "MigratedTxShards"
    TABLE_BLOCK_STORE_VOLUMES = "BlockStoreVolumes"
    TABLE_BLOCK_STORE_VOLUME_ALTERS = "BlockStoreVolumeAlters"
    TABLE_FILE_STORE_INFOS = "FileStoreInfos"
    TABLE_FILE_STORE_ALTERS = "FileStoreAlters"

    # kind -> (row table, alter table, version column).  The two protocols match
    # but the column names do not.
    VERSIONED_TABLES = {
        "blockstore": (TABLE_BLOCK_STORE_VOLUMES, TABLE_BLOCK_STORE_VOLUME_ALTERS,
                       "AlterVersion"),
        "filestore": (TABLE_FILE_STORE_INFOS, TABLE_FILE_STORE_ALTERS, "Version"),
    }

    def __init__(self, dump: TabletDump):
        self.dump = dump

    @property
    def tablet_id(self) -> int:
        return self.dump.tablet_id

    def shards(self) -> Iterator[Shard]:
        """Shards from both Shards and MigratedShards.

        MigratedShards carries an explicit owner, Shards is implicitly owned by
        this SchemeShard.
        """
        for row in self.dump.rows(self.TABLE_SHARDS):
            shard_idx = as_int(row.get("ShardIdx"))
            if shard_idx is None:
                continue
            yield Shard(
                owner_tablet_id=self.tablet_id,
                shard_idx=shard_idx,
                tablet_id=as_int(row.get("TabletId")),
                path_id=as_int(row.get("PathId")),
                tablet_type=as_int(row.get("TabletType")),
            )

        for row in self.dump.rows(self.TABLE_MIGRATED_SHARDS):
            shard_idx = as_int(row.get("LocalShardId"))
            if shard_idx is None:
                continue
            yield Shard(
                owner_tablet_id=as_int(row.get("OwnerShardId"), self.tablet_id) or self.tablet_id,
                shard_idx=shard_idx,
                tablet_id=as_int(row.get("TabletId")),
                path_id=as_int(row.get("LocalPathId")),
                tablet_type=as_int(row.get("TabletType")),
                migrated=True,
            )

    def paths(self) -> Iterator[Path]:
        for row in self.dump.rows(self.TABLE_PATHS):
            path_id = as_int(row.get("Id"))
            if path_id is None:
                continue
            yield Path(
                path_id=path_id,
                parent_id=as_int(row.get("ParentId")),
                name=str(row.get("Name") or ""),
                path_type=as_int(row.get("PathType")),
                step_dropped=as_int(row.get("StepDropped"), 0) or 0,
            )

    def sys_param(self, param_id: int) -> Optional[int]:
        """SysParams values are Utf8, so numbers arrive as decimal strings."""
        for row in self.dump.rows(self.TABLE_SYS_PARAMS):
            if as_int(row.get("Id")) == param_id:
                return as_int(row.get("Value"))
        return None

    def next_path_id(self) -> Optional[int]:
        return self.sys_param(SS_SYS_PARAM_NEXT_PATH_ID)

    def next_shard_idx(self) -> Optional[int]:
        return self.sys_param(SS_SYS_PARAM_NEXT_SHARD_IDX)

    def subdomains(self) -> List[Subdomain]:
        """Databases this SchemeShard owns, with their (unbacked) tablets."""
        shards_by_idx = {s.shard_idx: s for s in self.shards() if s.owner_tablet_id == self.tablet_id}
        names = {p.path_id: p.name for p in self.paths()}

        by_path: Dict[int, List[Shard]] = {}
        for row in self.dump.rows(self.TABLE_SUB_DOMAIN_SHARDS):
            path_id = as_int(row.get("PathId"))
            shard_idx = as_int(row.get("ShardIdx"))
            if path_id is None or shard_idx is None:
                continue
            shard = shards_by_idx.get(shard_idx)
            if shard is not None:
                by_path.setdefault(path_id, []).append(shard)

        # The root domain's path is its own parent (Paths row 1: parent 1).
        root_paths = {p.path_id for p in self.paths() if p.parent_id == p.path_id}

        result: List[Subdomain] = []
        for row in self.dump.rows(self.TABLE_SUB_DOMAINS):
            path_id = as_int(row.get("PathId"))
            if path_id is None:
                continue
            shared_hive = as_int(row.get("SharedHiveId"))
            # "No shared hive" is stored as InvalidTabletId, not as NULL or 0.
            if shared_hive in (0, None, INVALID_TABLET_ID):
                shared_hive = None
            result.append(
                Subdomain(
                    path_id=path_id,
                    name=names.get(path_id, "<path %d>" % path_id),
                    shared_hive_id=shared_hive,
                    shards=tuple(by_path.get(path_id, ())),
                    is_root_domain=path_id in root_paths,
                )
            )
        return result

    def databases(self) -> List[Subdomain]:
        """Tenant databases only, without the domain this SchemeShard serves."""
        return [s for s in self.subdomains() if not s.is_root_domain]

    def path_names(self) -> Dict[int, str]:
        """Full path per path id, as SchemeShard would print it.

        The root is its own parent, which terminates the walk; anything else
        that loops back on itself is broken metadata, so the walk is bounded
        rather than trusting the tree.
        """
        paths = {p.path_id: p for p in self.paths()}
        names: Dict[int, str] = {}
        for path_id in paths:
            parts: List[str] = []
            seen = set()
            cur = path_id
            while cur in paths and cur not in seen:
                seen.add(cur)
                node = paths[cur]
                parent = node.parent_id
                if parent is None or parent == cur:
                    # The root's own name already carries the leading slash.
                    parts.append(node.name.lstrip("/"))
                    break
                parts.append(node.name)
                cur = parent
            names[path_id] = "/" + "/".join(reversed(parts))
        return names

    def path_name(self, path_id: Optional[int]) -> str:
        if path_id is None:
            return "<no path>"
        return self.path_names().get(path_id, "<path %d>" % path_id)

    def txs_in_flight(self) -> List[TxInFlight]:
        """Schema operations SchemeShard had not finished yet.

        Rows are erased when an operation completes, so after a restore every
        row here is an operation the SchemeShard will pick up and drive again --
        against shards that have long since finished it.
        """
        shard_idxs: Dict[Tuple[int, int], List[int]] = {}
        for row in self.dump.rows(self.TABLE_TX_SHARDS):
            tx_id = as_int(row.get("TxId"))
            part_id = as_int(row.get("TxPartId"), 0)
            shard_idx = as_int(row.get("ShardIdx"))
            if tx_id is None or shard_idx is None:
                continue
            shard_idxs.setdefault((tx_id, part_id or 0), []).append(shard_idx)

        result: List[TxInFlight] = []
        for row in self.dump.rows(self.TABLE_TX_IN_FLIGHT):
            tx_id = as_int(row.get("TxId"))
            if tx_id is None:
                continue
            part_id = as_int(row.get("TxPartId"), 0) or 0
            result.append(
                TxInFlight(
                    tx_id=tx_id,
                    part_id=part_id,
                    tx_type=as_int(row.get("TxType")),
                    state=as_int(row.get("State")),
                    target_path_id=as_int(row.get("TargetPathId")),
                    min_step=as_int(row.get("MinStep")),
                    plan_step=as_int(row.get("PlanStep")),
                    shard_idxs=tuple(sorted(shard_idxs.get((tx_id, part_id), ()))),
                )
            )
        return result

    def versioned_objects(self) -> List[VersionedObject]:
        """BlockStore volumes and FileStores, with the versions in the backup.

        Both are configured by sending the version to the tablet, which refuses
        to go backwards, so these are the objects a rolled-back SchemeShard can
        no longer talk to.  The column names differ between the two
        (AlterVersion vs Version) although the protocols match.
        """
        result: List[VersionedObject] = []
        for kind, (table, alters, column) in sorted(self.VERSIONED_TABLES.items()):
            pending: Dict[int, Optional[int]] = {}
            for row in self.dump.rows(alters):
                path_id = as_int(row.get("PathId"))
                if path_id is not None:
                    pending[path_id] = as_int(row.get(column))
            for row in self.dump.rows(table):
                path_id = as_int(row.get("PathId"))
                if path_id is None:
                    continue
                result.append(
                    VersionedObject(
                        path_id=path_id,
                        kind=kind,
                        version=as_int(row.get(column)),
                        alter_version=pending.get(path_id),
                        has_alter=path_id in pending,
                    )
                )
        return result

    def tx_shard_keys(self, tx_id: int, part_id: int) -> List[Tuple[str, Dict[str, int]]]:
        """Primary keys of every shard row belonging to one sub-operation.

        Returns ``(table, key)`` pairs across all three spellings LoadTxShards
        reads.  A shard row whose operation is gone is not survivable --
        ``Y_VERIFY_S(txState, "There's shard for unknown Operation")`` in
        schemeshard__init.cpp -- so removing an operation means removing exactly
        this set.
        """
        keys: List[Tuple[str, Dict[str, int]]] = []

        for row in self.dump.rows(self.TABLE_TX_SHARDS):
            shard_idx = as_int(row.get("ShardIdx"))
            if (as_int(row.get("TxId")), as_int(row.get("TxPartId"), 0) or 0) != (tx_id, part_id):
                continue
            if shard_idx is not None:
                keys.append(
                    (self.TABLE_TX_SHARDS,
                     {"TxId": tx_id, "TxPartId": part_id, "ShardIdx": shard_idx})
                )

        # The pre-V2 table has no part column: its rows are always part 0.
        if part_id == 0:
            for row in self.dump.rows(self.TABLE_TX_SHARDS_V1):
                shard_idx = as_int(row.get("ShardIdx"))
                if as_int(row.get("TxId")) != tx_id or shard_idx is None:
                    continue
                keys.append((self.TABLE_TX_SHARDS_V1, {"TxId": tx_id, "ShardIdx": shard_idx}))

        for row in self.dump.rows(self.TABLE_MIGRATED_TX_SHARDS):
            owner = as_int(row.get("ShardOwnerId"))
            local = as_int(row.get("ShardLocalIdx"))
            if (as_int(row.get("TxId")), as_int(row.get("TxPartId"), 0) or 0) != (tx_id, part_id):
                continue
            if owner is None or local is None:
                continue
            keys.append(
                (self.TABLE_MIGRATED_TX_SHARDS,
                 {"TxId": tx_id, "TxPartId": part_id,
                  "ShardOwnerId": owner, "ShardLocalIdx": local})
            )

        return keys


# --------------------------------------------------------------------------
# BSController
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class VSlot:
    node_id: Optional[int]
    pdisk_id: Optional[int]
    vslot_id: Optional[int]
    group_id: Optional[int]
    group_generation: Optional[int]

    @property
    def ref(self) -> str:
        return "%s:%s:%s" % (self.node_id, self.pdisk_id, self.vslot_id)


@dataclass(frozen=True)
class PDisk:
    node_id: Optional[int]
    pdisk_id: Optional[int]
    guid: Optional[int]
    next_vslot_id: Optional[int]

    @property
    def ref(self) -> str:
        return "%s:%s" % (self.node_id, self.pdisk_id)


class BsControllerView:
    TABLE_GROUP = "Group"
    TABLE_STATE = "State"
    TABLE_VSLOT = "VSlot"
    TABLE_PDISK = "PDisk"

    def __init__(self, dump: TabletDump):
        self.dump = dump

    @property
    def tablet_id(self) -> int:
        return self.dump.tablet_id

    def group_ids(self) -> set:
        ids = set()
        for row in self.dump.rows(self.TABLE_GROUP):
            group_id = as_int(row.get("ID"))
            if group_id is not None:
                ids.add(group_id)
        return ids

    def next_group_id(self) -> Optional[int]:
        for row in self.dump.rows(self.TABLE_STATE):
            value = as_int(row.get("NextGroupID"))
            if value is not None:
                return value
        return None

    def vslots(self) -> Iterator[VSlot]:
        for row in self.dump.rows(self.TABLE_VSLOT):
            yield VSlot(
                node_id=as_int(row.get("NodeID")),
                pdisk_id=as_int(row.get("PDiskID")),
                vslot_id=as_int(row.get("VSlotID")),
                group_id=as_int(row.get("GroupID")),
                group_generation=as_int(row.get("GroupGeneration")),
            )

    def pdisks(self) -> Iterator[PDisk]:
        for row in self.dump.rows(self.TABLE_PDISK):
            yield PDisk(
                node_id=as_int(row.get("NodeID")),
                pdisk_id=as_int(row.get("PDiskID")),
                guid=as_int(row.get("Guid")),
                next_vslot_id=as_int(row.get("NextVSlotId")),
            )


def hive_view(state: Any) -> Optional[HiveView]:
    from .model import HIVE

    dump = state.one(HIVE)
    return HiveView(dump) if dump else None


def schemeshard_views(state: Any) -> List[SchemeShardView]:
    from .model import SCHEME_SHARD

    return [SchemeShardView(d) for d in state.by_type(SCHEME_SHARD)]


def bsc_view(state: Any) -> Optional[BsControllerView]:
    from .model import BS_CONTROLLER

    dump = state.one(BS_CONTROLLER)
    return BsControllerView(dump) if dump else None
