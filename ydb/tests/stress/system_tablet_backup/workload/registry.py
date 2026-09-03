# -*- coding: utf-8 -*-
"""Registry of load generators.

Adding a generator is one decorated factory::

    from ..registry import WorkloadContext, workload

    @workload("my_load", description="what it stresses", target_rps=1.0)
    def build(ctx: WorkloadContext):
        return MyWorkload(ctx)

and one line in ``generators/__init__.py``.  The runner then picks it up, the
CLI can enable or disable it by name and ``--rps name=value`` retunes it
without touching the code.
"""

from __future__ import annotations

import random
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Sequence

from .ledger import NullLedger
from .shared import LiveObjects


@dataclass
class WorkloadContext:
    """Everything a generator may need, so factories stay uniform."""

    client: Any
    stop: threading.Event
    database: str = "/Root"
    mon_endpoint: str = ""
    grpc_host: str = "localhost"
    grpc_port: int = 2135
    ledger: Any = field(default_factory=NullLedger)
    # Objects created by one generator and reused by another.
    live: LiveObjects = field(default_factory=LiveObjects)
    # Per-generator target rate, overriding the registered default.
    rps: Dict[str, float] = field(default_factory=dict)

    def target_rps(self, name: str, default: float) -> float:
        return self.rps.get(name, default)


@dataclass(frozen=True)
class WorkloadSpec:
    name: str
    factory: Callable[[WorkloadContext], Any]
    description: str = ""
    target_rps: Optional[float] = None
    # Off by default when it needs something the plain stand may not have.
    enabled_by_default: bool = True


_REGISTRY: Dict[str, WorkloadSpec] = {}


def workload(
    name: str,
    description: str = "",
    target_rps: Optional[float] = None,
    enabled_by_default: bool = True,
) -> Callable[[Callable[[WorkloadContext], Any]], Callable[[WorkloadContext], Any]]:
    def decorator(factory):
        if name in _REGISTRY:
            raise ValueError("duplicate workload name %r" % name)
        _REGISTRY[name] = WorkloadSpec(
            name=name,
            factory=factory,
            description=description,
            target_rps=target_rps,
            enabled_by_default=enabled_by_default,
        )
        return factory

    return decorator


def all_workloads() -> List[WorkloadSpec]:
    from . import generators  # noqa: F401  (import registers the generators)

    return sorted(_REGISTRY.values(), key=lambda spec: spec.name)


def select_workloads(
    only: Sequence[str] = (),
    exclude: Sequence[str] = (),
) -> List[WorkloadSpec]:
    specs = all_workloads()
    known = {spec.name for spec in specs}

    unknown = [n for n in list(only) + list(exclude) if n not in known]
    if unknown:
        raise ValueError(
            "unknown workload(s): %s (known: %s)"
            % (", ".join(sorted(unknown)), ", ".join(sorted(known)))
        )

    if only:
        specs = [s for s in specs if s.name in set(only)]
    else:
        specs = [s for s in specs if s.enabled_by_default]
    if exclude:
        specs = [s for s in specs if s.name not in set(exclude)]
    return specs


def build_workloads(specs: Sequence[WorkloadSpec], ctx: WorkloadContext) -> List[Any]:
    return [spec.factory(ctx) for spec in specs]


class Pacer:
    """Keeps a loop near a target rate.

    Sleeps for whatever is left of the slot after the operation, so a slow
    cluster degrades the rate instead of building an unbounded backlog.  The
    jitter keeps several threads from lining up on the same instant.
    """

    def __init__(self, target_rps: float, jitter: float = 0.2):
        self.period = 1.0 / target_rps if target_rps > 0 else 0.0
        self.jitter = jitter
        self._next = time.monotonic()

    def wait(self, stop: Optional[threading.Event] = None) -> None:
        if self.period <= 0:
            return

        self._next += self.period * (1.0 + random.uniform(-self.jitter, self.jitter))
        now = time.monotonic()
        if self._next < now:
            # Fell behind: give up on catching up rather than spinning.
            self._next = now
            return

        delay = self._next - now
        if stop is not None:
            stop.wait(delay)
        else:
            time.sleep(delay)
