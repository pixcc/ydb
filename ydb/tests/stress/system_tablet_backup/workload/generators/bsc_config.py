# -*- coding: utf-8 -*-
"""G4: BSController config transaction churn.

The production profile shows ``BSController/TTxConfigCmd`` at ~0.04/s.  Every
``TEvControllerConfigRequest`` runs through ``TTxConfigCmd`` regardless of
whether its commands read or write (``config_cmd.cpp``: the handler executes
the transaction before looking at the request), so a read-only command
reproduces the transaction rate without touching cluster topology.

That matters here: this workload runs alongside deliberate restores, and a
generator that reshuffled storage would make every finding ambiguous.
"""

from __future__ import annotations

import logging
import threading

from ydb.tests.library.clients.kikimr_client import KiKiMRMessageBusClient
from ydb.tests.stress.common.common import WorkloadBase

from ..registry import Pacer, WorkloadContext, workload

logger = logging.getLogger(__name__)

NAME = "bsc_config"

CONFIG_RPS = 0.04


class WorkloadBscConfig(WorkloadBase):
    def __init__(self, ctx: WorkloadContext):
        super().__init__(ctx.client, "", NAME, ctx.stop)
        self.ctx = ctx
        self._client = None
        self._lock = threading.Lock()
        self.requests = 0
        self.failed = 0

    def get_stat(self) -> str:
        with self._lock:
            return "config requests: %d, failed: %d" % (self.requests, self.failed)

    def _bump(self, field: str) -> None:
        with self._lock:
            setattr(self, field, getattr(self, field) + 1)

    def _pre_start(self) -> bool:
        try:
            self._client = KiKiMRMessageBusClient(self.ctx.grpc_host, self.ctx.grpc_port)
        except Exception as exc:
            logger.error("%s cannot reach BSController: %s", NAME, exc)
            return False
        return True

    def _config_loop(self) -> None:
        pacer = Pacer(self.ctx.target_rps(NAME, CONFIG_RPS))
        while not self.is_stop_requested():
            pacer.wait(self.stop)
            try:
                self._client.read_host_configs()
                self._bump("requests")
            except Exception as exc:
                self._bump("failed")
                logger.warning("BSController config request failed: %s", exc)

    def get_workload_thread_funcs(self):
        return [self._config_loop]


@workload(
    NAME,
    description="read-only BSController config requests: TTxConfigCmd churn",
    target_rps=CONFIG_RPS,
)
def build(ctx: WorkloadContext) -> WorkloadBscConfig:
    return WorkloadBscConfig(ctx)
