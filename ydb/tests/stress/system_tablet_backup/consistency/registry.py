# -*- coding: utf-8 -*-
"""Check registry and runner.

Adding a new invariant is a single decorated generator::

    from ..model import HIVE, SCHEME_SHARD, error
    from ..registry import check

    @check(
        id="I9",
        title="Every shard has a channel binding",
        needs={SCHEME_SHARD: ["Shards", "ChannelsBinding"]},
    )
    def shards_have_bindings(state):
        ...
        yield error("shard %d has no binding" % shard_idx, shard_idx=shard_idx)

``needs`` does double duty: it declares the requirements (a check is skipped,
not failed, when a slice is missing) and it tells the loader which tables to
read, so a production run does not pull whole multi-gigabyte dumps into memory
for checks that were filtered out.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable, Dict, Iterable, Iterator, List, Mapping, Optional, Sequence, Set, Tuple

from .model import ClusterState, Finding, Severity

CheckFunc = Callable[[ClusterState], Iterable[Finding]]


@dataclass(frozen=True)
class CheckSpec:
    id: str
    title: str
    func: CheckFunc
    # slice name -> tables the check reads
    needs: Mapping[str, Tuple[str, ...]] = field(default_factory=dict)
    # Free-form labels, usable with --tag on the command line.
    tags: Tuple[str, ...] = ()
    description: str = ""

    @property
    def requires(self) -> Tuple[str, ...]:
        return tuple(sorted(self.needs))

    def missing_slices(self, available: Set[str]) -> Tuple[str, ...]:
        return tuple(s for s in self.requires if s not in available)


_REGISTRY: Dict[str, CheckSpec] = {}


def check(
    id: str,
    title: str,
    needs: Optional[Mapping[str, Sequence[str]]] = None,
    tags: Sequence[str] = (),
) -> Callable[[CheckFunc], CheckFunc]:
    """Register a check.  The wrapped function is returned unchanged."""

    def decorator(func: CheckFunc) -> CheckFunc:
        if id in _REGISTRY:
            raise ValueError(
                "duplicate check id %r (already registered by %s)"
                % (id, _REGISTRY[id].func.__qualname__)
            )
        _REGISTRY[id] = CheckSpec(
            id=id,
            title=title,
            func=func,
            needs={slice_: tuple(tables) for slice_, tables in (needs or {}).items()},
            tags=tuple(tags),
            description=(func.__doc__ or "").strip(),
        )
        return func

    return decorator


def _natural_key(check_id: str) -> Tuple[str, int, str]:
    """Order I2 before I10 instead of lexicographically."""
    prefix = check_id.rstrip("0123456789")
    digits = check_id[len(prefix):]
    return (prefix, int(digits) if digits else 0, check_id)


def all_checks() -> List[CheckSpec]:
    """Every registered check, in natural id order."""
    # Importing the checks package populates the registry as a side effect.
    from . import checks  # noqa: F401  (circular by design, guarded by import order)

    return sorted(_REGISTRY.values(), key=lambda spec: _natural_key(spec.id))


def select_checks(
    only: Sequence[str] = (),
    exclude: Sequence[str] = (),
    tags: Sequence[str] = (),
) -> List[CheckSpec]:
    """Filter the registry by id and tag."""
    specs = all_checks()
    known = {spec.id for spec in specs}

    unknown = [i for i in list(only) + list(exclude) if i not in known]
    if unknown:
        raise ValueError(
            "unknown check id(s): %s (known: %s)"
            % (", ".join(sorted(unknown)), ", ".join(sorted(known)))
        )

    if only:
        specs = [s for s in specs if s.id in set(only)]
    if tags:
        wanted = set(tags)
        specs = [s for s in specs if wanted & set(s.tags)]
    if exclude:
        specs = [s for s in specs if s.id not in set(exclude)]
    return specs


def required_tables(specs: Iterable[CheckSpec]) -> Dict[str, Set[str]]:
    """Union of the tables the given checks read, per slice."""
    needed: Dict[str, Set[str]] = {}
    for spec in specs:
        for slice_, tables in spec.needs.items():
            needed.setdefault(slice_, set()).update(tables)
    return needed


@dataclass
class CheckOutcome:
    spec: CheckSpec
    findings: List[Finding] = field(default_factory=list)
    skipped_reason: Optional[str] = None
    failed_reason: Optional[str] = None

    @property
    def ok(self) -> bool:
        return not self.findings and self.skipped_reason is None and self.failed_reason is None

    @property
    def max_severity(self) -> Optional[Severity]:
        return max((f.severity for f in self.findings), default=None)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "id": self.spec.id,
            "title": self.spec.title,
            "requires": list(self.spec.requires),
            "tags": list(self.spec.tags),
            "skipped_reason": self.skipped_reason,
            "failed_reason": self.failed_reason,
            "findings": [f.to_dict() for f in self.findings],
        }


def run_checks(
    state: ClusterState,
    specs: Optional[Sequence[CheckSpec]] = None,
) -> List[CheckOutcome]:
    """Run ``specs`` (default: everything registered) against ``state``.

    A check whose requirements are not satisfied is skipped with a reason
    rather than failed -- that is what lets ledger-backed checks coexist with
    production runs that have no ledger.
    """
    if specs is None:
        specs = all_checks()

    available = state.slices()
    outcomes: List[CheckOutcome] = []

    for spec in specs:
        outcome = CheckOutcome(spec=spec)
        missing = spec.missing_slices(available)
        if missing:
            outcome.skipped_reason = "no state for: %s" % ", ".join(missing)
            outcomes.append(outcome)
            continue

        try:
            for finding in spec.func(state) or ():
                finding.check_id = spec.id
                outcome.findings.append(finding)
        except Exception as exc:  # a broken check must not hide the others
            outcome.failed_reason = "%s: %s" % (type(exc).__name__, exc)

        outcomes.append(outcome)

    return outcomes


def iter_findings(outcomes: Iterable[CheckOutcome]) -> Iterator[Finding]:
    for outcome in outcomes:
        for finding in outcome.findings:
            yield finding
