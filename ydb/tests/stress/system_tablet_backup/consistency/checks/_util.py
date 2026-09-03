# -*- coding: utf-8 -*-
"""Helpers shared by the checks."""

from __future__ import annotations

from typing import Any, Callable, Iterable, Iterator, List, Sequence

from ..model import Finding, Severity

# A rolled-back tablet can produce hundreds of thousands of identical findings.
# Report the first few in full and collapse the rest into one summary line.
DEFAULT_SAMPLE_LIMIT = 20


def capped(
    items: Iterable[Any],
    make_finding: Callable[[Any], Finding],
    summary: Callable[[int, List[Any]], Finding],
    limit: int = DEFAULT_SAMPLE_LIMIT,
) -> Iterator[Finding]:
    """Yield at most ``limit`` detailed findings, then a summary for the rest.

    ``summary`` receives the total count and the overflow items so it can put a
    machine-readable sample into the finding details.
    """
    shown = 0
    total = 0
    overflow: List[Any] = []

    for item in items:
        total += 1
        if shown < limit:
            shown += 1
            yield make_finding(item)
        else:
            overflow.append(item)

    if overflow:
        yield summary(total, overflow)


def worst(findings: Sequence[Finding]) -> Severity:
    return max((f.severity for f in findings), default=Severity.INFO)
