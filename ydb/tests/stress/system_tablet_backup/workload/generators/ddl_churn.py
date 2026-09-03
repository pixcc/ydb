# -*- coding: utf-8 -*-
"""G1: schema operation churn -- the main source of cross-tablet references.

Mirrors the production profile measured on an NBS cluster, where a volume is
created roughly once a second.  Locally a table plays the role of the volume:
the tablet chain it exercises is the same one.

    SchemeShard   TTxOperationPropose, TxProgressOp, TxPlanStep,
                  TxCreateTabletReply, TxFreeTabletResult, TxCleanDroppedPaths
    Hive          TxCreateTablet, TxUpdateTabletGroups, TxStartTablet,
                  TxDeleteTablet
    BSController  group allocation for the new tablet's channels

Target rates come from the measured profile: ~2.2 creates/s against ~1.2
drops/s, so the object count grows slowly over a run.
"""

from __future__ import annotations

import itertools
import logging
import threading
import time
from typing import List, Optional
from urllib.parse import urlencode

import requests
import ydb

from ydb.tests.stress.common.common import WorkloadBase

from ..registry import Pacer, WorkloadContext, workload
from ..shared import CreatedObject, LiveObjects

logger = logging.getLogger(__name__)

NAME = "ddl_churn"
DIRECTORY = "ddl_churn"

CREATE_RPS = 2.2
DROP_RPS = 1.2
# One partition per table keeps propose:create_tablet at the ~1:1 the profile
# shows.  Raise it to grow the tablet count faster.
PARTITIONS = 1
# Below this many live objects the dropper waits, so a run always leaves the
# cluster with something to be inconsistent about.
MIN_LIVE_OBJECTS = 20


class WorkloadDdlChurn(WorkloadBase):
    def __init__(self, ctx: WorkloadContext):
        super().__init__(ctx.client, "", NAME, ctx.stop)
        self.ctx = ctx
        self.live: LiveObjects = ctx.live
        self.ledger = ctx.ledger
        self.partitions = PARTITIONS

        self._counter = itertools.count()
        self._lock = threading.Lock()
        self.created = 0
        self.dropped = 0
        self.failed = 0

    # -- reporting ---------------------------------------------------------

    def get_stat(self) -> str:
        with self._lock:
            return "created: %d, dropped: %d, live: %d, failed: %d" % (
                self.created,
                self.dropped,
                self.live.count(),
                self.failed,
            )

    def _bump(self, field: str) -> None:
        with self._lock:
            setattr(self, field, getattr(self, field) + 1)

    # -- helpers -----------------------------------------------------------

    def _path(self, name: str) -> str:
        return "%s/%s/%s" % (self.ctx.database.rstrip("/"), DIRECTORY, name)

    def _describe(self, path: str) -> Optional[CreatedObject]:
        """Resolve PathId and shard tablet ids through the viewer.

        The Python SDK's describe does not expose them, and they are exactly
        the cross-tablet references the checks compare.
        """
        if not self.ctx.mon_endpoint:
            return None

        url = "%s/viewer/json/describe?%s" % (
            self.ctx.mon_endpoint.rstrip("/"),
            urlencode({"database": self.ctx.database, "path": path, "enums": "true", "subs": "0"}),
        )
        try:
            response = requests.get(url, timeout=30)
            response.raise_for_status()
            body = response.json()
        except Exception as exc:
            logger.warning("describe %s failed: %s", path, exc)
            return None

        description = body.get("PathDescription") or {}
        own = description.get("Self") or {}
        tablet_ids: List[int] = []
        for partition in description.get("TablePartitions") or []:
            datashard = partition.get("DatashardId")
            if datashard is not None:
                tablet_ids.append(int(datashard))

        return CreatedObject(
            path=path,
            path_id=int(own["PathId"]) if own.get("PathId") is not None else None,
            schemeshard_id=int(own["SchemeshardId"]) if own.get("SchemeshardId") is not None else None,
            tablet_ids=tuple(tablet_ids),
        )

    # -- loops -------------------------------------------------------------

    def _create_once(self) -> None:
        name = "t_%d_%d" % (int(time.time()), next(self._counter))
        path = self._path(name)

        self.client.query(
            """
            CREATE TABLE `%s` (
                id Uint64 NOT NULL,
                payload String,
                PRIMARY KEY (id)
            ) WITH (
                AUTO_PARTITIONING_BY_SIZE = DISABLED,
                UNIFORM_PARTITIONS = %d
            )
            """ % (path, self.partitions),
            True,
        )

        obj = self._describe(path)
        if obj is None:
            obj = CreatedObject(path=path, path_id=None, schemeshard_id=None)

        self.live.add(obj)
        self._bump("created")
        self.ledger.record(
            "create",
            path=path,
            path_id=obj.path_id,
            schemeshard_id=obj.schemeshard_id,
            shards=[{"tablet_id": tablet_id} for tablet_id in obj.tablet_ids],
        )

    def _create_loop(self) -> None:
        pacer = Pacer(self.ctx.target_rps(NAME + ".create", CREATE_RPS))
        while not self.is_stop_requested():
            try:
                self._create_once()
            except (ydb.Unavailable, ydb.ConnectionLost, ydb.Overloaded, ydb.GenericError) as exc:
                self._bump("failed")
                self.ledger.record("create", status="error", error=str(exc))
            pacer.wait(self.stop)

    def _drop_loop(self) -> None:
        pacer = Pacer(self.ctx.target_rps(NAME + ".drop", DROP_RPS))
        while not self.is_stop_requested():
            pacer.wait(self.stop)
            if self.live.count() <= MIN_LIVE_OBJECTS:
                continue

            obj = self.live.take_random()
            if obj is None:
                continue

            try:
                self.client.query("DROP TABLE `%s`" % obj.path, True)
                self._bump("dropped")
                self.ledger.record("drop", path=obj.path, path_id=obj.path_id)
            except (ydb.Unavailable, ydb.ConnectionLost, ydb.Overloaded, ydb.GenericError) as exc:
                # Put it back: the object may well still exist.
                self.live.add(obj)
                self._bump("failed")
                self.ledger.record("drop", status="error", path=obj.path, error=str(exc))

    def get_workload_thread_funcs(self):
        return [self._create_loop, self._drop_loop]


@workload(
    NAME,
    description="create/drop tables: SchemeShard -> Hive -> BSController reference churn",
    target_rps=CREATE_RPS,
)
def build(ctx: WorkloadContext) -> WorkloadDdlChurn:
    return WorkloadDdlChurn(ctx)
