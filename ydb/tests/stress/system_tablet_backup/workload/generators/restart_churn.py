# -*- coding: utf-8 -*-
"""G2: tablet restart churn.

The production profile shows ``Hive/TxStartTablet`` at 8.2/s against
``TxCreateTablet`` at 2.1/s -- tablets are started about four times more often
than they are created, from restarts and moves.  Without this generator Hive
never rewrites ``Tablet::KnownGeneration`` or ``TabletChannelGen``, and those
are precisely the rows whose staleness stops a restored tablet from booting.

Restarts go through the tablet monitoring page, the same entry point the
recovery guide uses.
"""

from __future__ import annotations

import logging
import threading

import requests

from ydb.tests.stress.common.common import WorkloadBase

from ..registry import Pacer, WorkloadContext, workload

logger = logging.getLogger(__name__)

NAME = "restart_churn"

# 8.2/s total minus the starts that creations already cause.
RESTART_RPS = 6.0


class WorkloadRestartChurn(WorkloadBase):
    def __init__(self, ctx: WorkloadContext):
        super().__init__(ctx.client, "", NAME, ctx.stop)
        self.ctx = ctx
        self._lock = threading.Lock()
        self.restarted = 0
        self.skipped = 0
        self.failed = 0

    def get_stat(self) -> str:
        with self._lock:
            return "restarted: %d, skipped: %d, failed: %d" % (
                self.restarted,
                self.skipped,
                self.failed,
            )

    def _bump(self, field: str) -> None:
        with self._lock:
            setattr(self, field, getattr(self, field) + 1)

    def _restart_loop(self) -> None:
        pacer = Pacer(self.ctx.target_rps(NAME, RESTART_RPS))
        while not self.is_stop_requested():
            pacer.wait(self.stop)

            tablet_id = self.ctx.live.random_tablet()
            if tablet_id is None:
                # Nothing created yet; the DDL generator has not caught up.
                self._bump("skipped")
                continue

            url = "%s/tablets?RestartTabletID=%d" % (self.ctx.mon_endpoint.rstrip("/"), tablet_id)
            try:
                response = requests.get(url, timeout=30)
                response.raise_for_status()
                self._bump("restarted")
            except Exception as exc:
                self._bump("failed")
                logger.warning("restart of tablet %d failed: %s", tablet_id, exc)

    def _pre_start(self) -> bool:
        if not self.ctx.mon_endpoint:
            logger.error("%s needs --mon-endpoint, not started", NAME)
            return False
        return True

    def get_workload_thread_funcs(self):
        return [self._restart_loop]


@workload(
    NAME,
    description="restart random tablets: Hive TxStartTablet / KnownGeneration churn",
    target_rps=RESTART_RPS,
)
def build(ctx: WorkloadContext) -> WorkloadRestartChurn:
    return WorkloadRestartChurn(ctx)
