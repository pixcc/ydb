# -*- coding: utf-8 -*-
from ydb.tests.library.harness.kikimr_runner import KiKiMR
from ydb.tests.library.harness.kikimr_config import KikimrConfigGenerator
from ydb.tests.library.common.types import Erasure
from ydb.tests.stress.common.common import YdbClient
from ydb.tests.stress.reconfig_state_storage_workload.workload import WorkloadRunner
from ydb.tests.library.harness.util import LogLevels


class TestReconfigStateStorageWorkload(object):

    @classmethod
    def setup_class(cls):
        cls.cluster = KiKiMR(KikimrConfigGenerator(
            erasure=Erasure.BLOCK_4_2,
            use_self_management=True,
            simple_config=True,
            metadata_section={
                "kind": "MainConfig",
                "version": 0,
                "cluster": "",
            },
            column_shard_config={
                "allow_nullable_columns_in_pk": True,
            },
            additional_log_configs={
                'BS_NODE': LogLevels.DEBUG,
                'STATESTORAGE': LogLevels.DEBUG,
            }
        ))
        cls.cluster.start()

    @classmethod
    def teardown_class(cls):
        cls.cluster.stop()

    def test(self):
        client = YdbClient(f'grpc://localhost:{self.cluster.nodes[1].grpc_port}', '/Root', True)
        client.wait_connection()
        with WorkloadRunner(client, self.cluster, 'reconfig_state_storage_workload', 120, True) as runner:
            runner.run()
