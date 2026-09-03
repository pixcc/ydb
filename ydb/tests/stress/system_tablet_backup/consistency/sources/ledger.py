# -*- coding: utf-8 -*-
"""Reader for the workload ledger.

The ledger is an append-only JSONL file written by the stress workload outside
the cluster, recording what it actually asked for and what the cluster answered::

    {"ts": 1786974500.12, "op": "create", "status": "ok", "path": "/Root/t_41",
     "path_id": 41, "shards": [{"shard_idx": 77, "tablet_id": 72075186224037965}]}
    {"ts": 1786974502.44, "op": "drop", "status": "ok", "path": "/Root/t_12"}

It exists only on a test stand.  Checks that need it declare ``LEDGER`` in
``needs`` and are skipped -- not failed -- on production.
"""

from __future__ import annotations

import json
import os
from typing import List, Optional

from ..model import Ledger, LedgerEntry


def load_ledger(path: str) -> Optional[Ledger]:
    """Parse a ledger file, tolerating a truncated last line.

    The workload may be killed mid-write, so a partial final record is expected
    and ignored rather than treated as an error.
    """
    if not path or not os.path.isfile(path):
        return None

    entries: List[LedgerEntry] = []
    with open(path, "r") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if not isinstance(record, dict):
                continue
            payload = {k: v for k, v in record.items() if k not in ("ts", "op")}
            entries.append(
                LedgerEntry(
                    ts=float(record.get("ts", 0.0) or 0.0),
                    op=str(record.get("op", "")),
                    payload=payload,
                )
            )

    return Ledger(entries=entries, source=path)
