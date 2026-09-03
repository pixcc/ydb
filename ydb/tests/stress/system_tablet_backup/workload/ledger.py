# -*- coding: utf-8 -*-
"""Append-only record of what the workload actually did.

The ledger lives outside the cluster and is the only witness that survives a
restore of *any* tablet, which is what makes it useful as an oracle: after
rolling Hive and SchemeShard back at once, neither of them remembers the object
but the ledger still does.

One JSON object per line, see ``consistency/sources/ledger.py`` for the reader.
"""

from __future__ import annotations

import json
import os
import threading
import time
from typing import Any, Dict, Optional


class LedgerWriter:
    """Thread-safe JSONL appender.

    Every workload thread writes through one instance, so the lock is on the
    hot path -- but at the rates this workload runs (a few operations per
    second) that is irrelevant next to the round trip to the cluster.
    """

    def __init__(self, path: str, flush_every_record: bool = True):
        self.path = path
        self._flush_every_record = flush_every_record
        self._lock = threading.Lock()

        directory = os.path.dirname(os.path.abspath(path))
        if directory:
            os.makedirs(directory, exist_ok=True)
        self._handle = open(path, "a", buffering=1)

    def record(self, op: str, status: str = "ok", **payload: Any) -> None:
        entry: Dict[str, Any] = {"ts": time.time(), "op": op, "status": status}
        entry.update(payload)
        line = json.dumps(entry, sort_keys=True)

        with self._lock:
            self._handle.write(line + "\n")
            if self._flush_every_record:
                # The point of the ledger is to survive a crash of the very
                # thing it is recording, so durability beats throughput here.
                self._handle.flush()
                os.fsync(self._handle.fileno())

    def close(self) -> None:
        with self._lock:
            if not self._handle.closed:
                self._handle.flush()
                os.fsync(self._handle.fileno())
                self._handle.close()

    def __enter__(self) -> "LedgerWriter":
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()


class NullLedger:
    """Stand-in used when no ledger path was configured."""

    path: Optional[str] = None

    def record(self, op: str, status: str = "ok", **payload: Any) -> None:
        pass

    def close(self) -> None:
        pass

    def __enter__(self) -> "NullLedger":
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        pass


def open_ledger(path: Optional[str]):
    return LedgerWriter(path) if path else NullLedger()
