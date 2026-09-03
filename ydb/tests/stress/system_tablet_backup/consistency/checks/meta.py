# -*- coding: utf-8 -*-
"""Checks about the backups themselves rather than their contents.

Restoring one tablet from a point in time far from the others is the root cause
of every referential finding, so it is worth stating up front -- but honestly.

**What is not a freshness measure.** The timestamp in the backup directory name
is when the *snapshot started*.  Backups are not taken on a timer: the changelog
keeps receiving commits for as long as the backup stays current, and a new
snapshot is only cut once the changelog outgrows it.  So a directory named 11:00
can hold state from 12:00, and comparing directory names across tablets says
nothing about which state is more recent.

**What the format offers.** Changelog commits carry a tablet ``step``, not a
wall clock (see the writer in ``flat_executor_backup.cpp``), and steps of
different tablets are incomparable.  The only wall-clock signal left is the
mtime of ``changelog.json`` -- when that tablet last recorded a change.  It is
filesystem metadata, so it survives ``rsync -a`` and ``scp -p`` but a plain
``scp -r`` resets it to the copy time.

For a precise, transfer-proof answer on a test stand, see ``I14``, which dates
each tablet's state against the workload ledger.
"""

from __future__ import annotations

import time
from typing import Iterator, Optional

from ..model import ClusterState, Finding, info, warning
from ..registry import check

# Below this, ordinary snapshot and flush scheduling explains the gap.
FRESHNESS_WARN_SECONDS = 300

# Standing caveat on the small-spread verdict.  A healthy cluster (snapshots cut
# at different times, every changelog current) and a copy that dropped
# timestamps produce the same picture, so the limitation is stated rather than
# guessed at.
MTIME_CAVEAT = (
    "relies on preserved mtimes -- a plain `scp -r` would flatten this to zero; "
    "use `rsync -a` or `scp -p`, or the ledger-based I14"
)


def _fmt(seconds: Optional[float]) -> str:
    if seconds is None:
        return "unknown"
    return time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime(seconds))


@check(
    id="I11",
    title="Tablet states are close together in time",
    needs={},
    tags=("meta",),
)
def state_freshness_spread(state: ClusterState) -> Iterator[Finding]:
    """Reports how stale each tablet's state is relative to the freshest one.

    A spread is not a defect by itself -- backups of different tablets are taken
    independently and are explicitly not mutually consistent -- but it bounds
    how much divergence the referential checks can legitimately find.
    """
    if len(state.dumps) < 2:
        return

    ordered = sorted(state.dumps, key=lambda d: d.tablet_type)
    snapshot_lines = ", ".join(
        "%s snapshot started %s" % (d.tablet_type, _fmt(d.snapshot_started_at)) for d in ordered
    )

    dated = [d for d in state.dumps if d.changelog_mtime is not None]
    if len(dated) < 2:
        yield info(
            "cannot date tablet states: changelog.json is missing for %d of %d tablets. "
            "Snapshot start times are a lower bound only (%s)"
            % (len(state.dumps) - len(dated), len(state.dumps), snapshot_lines),
            snapshots={d.tablet_type: _fmt(d.snapshot_started_at) for d in ordered},
        )
        return

    newest = max(d.changelog_mtime for d in dated)
    lags = sorted(
        ((newest - d.changelog_mtime, d) for d in dated),
        key=lambda item: item[0],
        reverse=True,
    )
    worst_lag = lags[0][0]

    detail = {
        d.tablet_type: {
            "lag_seconds": int(newest - d.changelog_mtime),
            "changelog_last_written": _fmt(d.changelog_mtime),
            "snapshot_started": _fmt(d.snapshot_started_at),
            "changelog_commits": d.changelog_commits,
            "generation": d.generation,
            "step": d.step,
        }
        for _, d in lags
    }

    message = "state freshness spread is %d s across %d tablets, by changelog mtime (%s)" % (
        int(worst_lag),
        len(dated),
        ", ".join("%s -%ds" % (d.tablet_type, int(lag)) for lag, d in lags),
    )

    if worst_lag >= FRESHNESS_WARN_SECONDS:
        yield warning(message, **detail)
    else:
        # A near-zero spread is the expected picture for a healthy cluster and
        # also what a timestamp-dropping copy looks like.  The two are not
        # distinguishable from the files, so the caveat travels with the verdict.
        yield info("%s; %s" % (message, MTIME_CAVEAT), **detail)


@check(
    id="I13",
    title="No backup has a truncated changelog",
    needs={},
    tags=("meta",),
)
def changelog_complete(state: ClusterState) -> Iterator[Finding]:
    """A truncated changelog means the most recent changes never reached the
    backup, so the restored tablet is older than its name suggests."""
    for dump in state.dumps:
        if dump.changelog_truncated:
            yield warning(
                "%s: changelog tail is unparsable, the last changes before the "
                "crash are missing (%d commits applied)"
                % (dump.label, dump.changelog_commits),
                tablet_type=dump.tablet_type,
                tablet_id=dump.tablet_id,
                changelog_commits=dump.changelog_commits,
                source=dump.source,
            )
