# -*- coding: utf-8 -*-
"""Rendering of check results."""

from __future__ import annotations

import json
from typing import Any, Dict, List, Sequence

from .model import ClusterState, Severity
from .registry import CheckOutcome

_MARK = {
    Severity.INFO: "info",
    Severity.WARNING: "WARN",
    Severity.ERROR: "FAIL",
    Severity.CRITICAL: "CRIT",
}


def summarize(outcomes: Sequence[CheckOutcome]) -> Dict[str, int]:
    counts = {severity.name.lower(): 0 for severity in Severity}
    counts.update({"checks": len(outcomes), "skipped": 0, "broken": 0})
    for outcome in outcomes:
        if outcome.skipped_reason:
            counts["skipped"] += 1
        if outcome.failed_reason:
            counts["broken"] += 1
        for finding in outcome.findings:
            counts[finding.severity.name.lower()] += 1
    return counts


def max_severity(outcomes: Sequence[CheckOutcome]) -> Severity:
    best = Severity.INFO
    for outcome in outcomes:
        for finding in outcome.findings:
            if finding.severity > best:
                best = finding.severity
    return best


def render_text(
    state: ClusterState,
    outcomes: Sequence[CheckOutcome],
    notes: Sequence[str] = (),
    verbose: bool = False,
) -> str:
    lines: List[str] = []

    lines.append("state: %s" % state.describe())
    for dump in sorted(state.dumps, key=lambda d: d.tablet_type):
        lines.append(
            "  %-14s %s  (%d changelog commits) %s"
            % (dump.tablet_type, dump.source, dump.changelog_commits,
               "[changelog truncated]" if dump.changelog_truncated else "")
        )
    for note in notes:
        lines.append("  note: %s" % note)
    lines.append("")

    for outcome in outcomes:
        header = "%-5s %s" % (outcome.spec.id, outcome.spec.title)

        if outcome.skipped_reason:
            lines.append("SKIP  %s -- %s" % (header, outcome.skipped_reason))
            continue
        if outcome.failed_reason:
            lines.append("BROKE %s -- check itself raised: %s" % (header, outcome.failed_reason))
            continue
        if not outcome.findings:
            lines.append("ok    %s" % header)
            continue

        worst = max(f.severity for f in outcome.findings)
        lines.append("%-5s %s" % (_MARK[worst], header))
        for finding in outcome.findings:
            lines.append("        [%s] %s" % (_MARK[finding.severity], finding.message))
            if verbose and finding.details:
                lines.append("              %s" % json.dumps(finding.details, sort_keys=True))

    counts = summarize(outcomes)
    lines.append("")
    lines.append(
        "%d checks: %d critical, %d error, %d warning, %d info, %d skipped, %d broken"
        % (
            counts["checks"],
            counts["critical"],
            counts["error"],
            counts["warning"],
            counts["info"],
            counts["skipped"],
            counts["broken"],
        )
    )

    return "\n".join(lines)


def render_json(
    state: ClusterState,
    outcomes: Sequence[CheckOutcome],
    notes: Sequence[str] = (),
) -> Dict[str, Any]:
    return {
        "state": {
            "tablets": [
                {
                    "tablet_type": d.tablet_type,
                    "tablet_id": d.tablet_id,
                    "generation": d.generation,
                    "step": d.step,
                    "snapshot_started_at": d.snapshot_started_at,
                    "changelog_mtime": d.changelog_mtime,
                    "source": d.source,
                    "changelog_commits": d.changelog_commits,
                    "changelog_truncated": d.changelog_truncated,
                }
                for d in sorted(state.dumps, key=lambda d: d.tablet_type)
            ],
            "ledger_entries": len(state.ledger) if state.ledger else None,
        },
        "notes": list(notes),
        "checks": [outcome.to_dict() for outcome in outcomes],
        "summary": summarize(outcomes),
        "max_severity": max_severity(outcomes).name,
    }
