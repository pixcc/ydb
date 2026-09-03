# -*- coding: utf-8 -*-
"""G5: node registration -- load on NodeBroker.

Unchanged behaviour, moved out of ``workload/__init__.py`` so every generator
lives behind the same registry.
"""

from __future__ import annotations

import threading
import time

import ydb
from ydb.public.api.grpc import ydb_discovery_v1_pb2_grpc
from ydb.public.api.protos import ydb_discovery_pb2

from ydb.tests.stress.common.common import WorkloadBase

from ..registry import WorkloadContext, workload

NAME = "register_node"

# Hosts are named so the validator can tell them from real cluster nodes.
FAKE_HOST_PREFIX = "system.tablet.backup.fake."
THREADS = 10


class WorkloadRegisterNode(WorkloadBase):
    def __init__(self, client, stop):
        super().__init__(client, "", NAME, stop)
        self.registered = 0
        self.next_id = 0
        self.lock = threading.Lock()

    def get_stat(self):
        with self.lock:
            return f"Registered: {self.registered}"

    def _get_next_id(self):
        with self.lock:
            node_id = self.next_id
            self.next_id += 1
            return node_id

    def _register_node(self, node_id):
        request = ydb_discovery_pb2.NodeRegistrationRequest(
            host=FAKE_HOST_PREFIX + str(node_id),
            port=19001,
            resolve_host=FAKE_HOST_PREFIX + str(node_id),
            address="594f:10c7:ad54:eada:99eb:7b5b:eec2:0000",
            location=ydb_discovery_pb2.NodeLocation(
                data_center="DC",
                module="1",
                rack="2",
                unit="3",
            ),
            path=self.client.database,
        )

        self.client.driver(
            request,
            ydb_discovery_v1_pb2_grpc.DiscoveryServiceStub,
            "NodeRegistration",
            ydb.operation.Operation,
            None,
            (self.client.driver,),
        )

    def _register_node_loop(self):
        while not self.is_stop_requested():
            try:
                self._register_node(self._get_next_id())
                with self.lock:
                    self.registered += 1
            except (ydb.Unavailable, ydb.ConnectionLost, ydb.GenericError):
                time.sleep(1)

    def get_workload_thread_funcs(self):
        return [self._register_node_loop for x in range(0, THREADS)]


@workload(NAME, description="register fake dynamic nodes: NodeBroker load")
def build(ctx: WorkloadContext) -> WorkloadRegisterNode:
    return WorkloadRegisterNode(ctx.client, ctx.stop)
