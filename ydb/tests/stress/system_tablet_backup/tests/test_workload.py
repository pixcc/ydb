# -*- coding: utf-8 -*-
import os
import time
import yatest
import pytest

from ydb.tests.library.common.types import Erasure
from ydb.tests.library.harness.util import LogLevels
from ydb.tests.library.stress.fixtures import StressFixture

from ydb.tests.stress.system_tablet_backup.consistency import Severity
from ydb.tests.stress.system_tablet_backup.consistency.registry import (
    required_tables,
    run_checks,
    select_checks,
)
from ydb.tests.stress.system_tablet_backup.consistency.report import render_text
from ydb.tests.stress.system_tablet_backup.consistency.sources import load_ledger, load_state

# Backup changelogs are written asynchronously, so a check run right after the
# last operation can see one tablet ahead of another.  Let them drain before
# comparing.
CHANGELOG_SETTLE_SECONDS = 20


class TestSystemTabletBackup(StressFixture):
    @pytest.fixture(autouse=True, scope="function")
    def setup(self, request):
        self._backup_path = yatest.common.output_path(
            f"system_tablet_backup_{request.node.name}")

        yield from self.setup_cluster(
            erasure=Erasure.MIRROR_3_DC,
            extra_feature_flags=['enable_configured_bootstrapper'],
            additional_log_configs={
                "LOCAL_DB_BACKUP": LogLevels.TRACE,
                "NODE_BROKER": LogLevels.TRACE,
                "BOOTSTRAPPER": LogLevels.TRACE,
            },
            system_tablet_backup_config={
                "filesystem": {
                    "path": self._backup_path,
                },
            },
        )

    def _run_workload(self, backup_path=None, ledger_path=None, workloads=()):
        cmd = [
            yatest.common.binary_path(os.getenv("YDB_TEST_PATH")),
            "--endpoint", f"grpc://localhost:{self.cluster.nodes[1].grpc_port}",
            "--mon-endpoint", f"http://localhost:{self.cluster.nodes[1].mon_port}",
            "--database", self.database,
            "--duration", self.base_duration,
        ]
        if backup_path is not None:
            cmd += ["--backup-path", backup_path]
        if ledger_path is not None:
            cmd += ["--ledger", ledger_path]
        for name in workloads:
            cmd += ["--workload", name]
        yatest.common.execute(cmd, wait=True)

    def _check_consistency(self, ledger_path=None):
        """Run every registered invariant against the produced backups."""
        specs = select_checks()
        needed = {
            slice_: tables
            for slice_, tables in required_tables(specs).items()
            if slice_ != "ledger"
        }
        state, notes = load_state(root=self._backup_path, needed_tables=needed)
        assert state.dumps, "no system tablet backups were produced: %s" % notes

        if ledger_path:
            state.ledger = load_ledger(ledger_path)

        outcomes = run_checks(state, specs)
        report = render_text(state, outcomes, notes, verbose=True)

        broken = [o for o in outcomes if o.failed_reason]
        assert not broken, "check raised:\n%s" % report

        return outcomes, report

    def test_workload(self):
        self._run_workload(backup_path=self._backup_path)

    def test_workload_with_corrupted_backup(self):
        tablet_id = 72057594037936129  # NODE_BROKER
        corrupted_backup_dir = os.path.join(
            self._backup_path, "node_broker", str(tablet_id),
            "backup_corrupted", "snapshot")
        os.makedirs(corrupted_backup_dir)

        with pytest.raises(yatest.common.process.ExecutionError):
            self._run_workload(backup_path=self._backup_path)

    def test_consistency_baseline(self):
        """No restore happens here: the cluster must be self-consistent.

        This is the control for the stale-restore scenarios -- any finding it
        produces is a false positive in a check, not a real inconsistency.
        """
        ledger_path = yatest.common.output_path("ledger.jsonl")
        self._run_workload(ledger_path=ledger_path)

        time.sleep(CHANGELOG_SETTLE_SECONDS)

        outcomes, report = self._check_consistency(ledger_path)

        problems = [
            finding
            for outcome in outcomes
            for finding in outcome.findings
            if finding.severity >= Severity.ERROR
        ]
        assert not problems, "consistent cluster reported findings:\n%s" % report
