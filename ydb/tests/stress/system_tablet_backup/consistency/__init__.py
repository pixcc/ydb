# -*- coding: utf-8 -*-
"""Cross-tablet consistency checker for YDB cluster system tablets.

This package is intentionally **standalone**: it depends on the Python standard
library only, so the directory can be copied to a production host and run there
as ``python3 -m consistency`` without installing the YDB SDK, yatest or pytest.

State is read from system tablet backups (``system_tablet_backup_config``), whose
snapshots are plain NDJSON dumps of every local database table.  That makes the
same code usable both in the stress test (where a ledger of performed operations
is available) and on production (where it is not) -- checks declare what they
need and are skipped with an explicit reason when it is missing.

Entry points:
    ``consistency.model``     -- state model and findings
    ``consistency.registry``  -- check registry and runner
    ``consistency.sources``   -- state loaders (backup snapshots, ledger)
    ``consistency.checks``    -- the invariants themselves
"""

from .model import (  # noqa: F401
    BS_CONTROLLER,
    HIVE,
    LEDGER,
    NODE_BROKER,
    SCHEME_SHARD,
    ClusterState,
    Finding,
    Severity,
    TabletDump,
    critical,
    error,
    info,
    warning,
)
from .registry import CheckSpec, all_checks, check, run_checks  # noqa: F401

__all__ = [
    "BS_CONTROLLER",
    "HIVE",
    "LEDGER",
    "NODE_BROKER",
    "SCHEME_SHARD",
    "CheckSpec",
    "ClusterState",
    "Finding",
    "Severity",
    "TabletDump",
    "all_checks",
    "check",
    "critical",
    "error",
    "info",
    "run_checks",
    "warning",
]
