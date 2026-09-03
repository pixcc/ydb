# -*- coding: utf-8 -*-
"""State shared between load generators.

The DDL generator knows which objects exist; the restart generator needs that
same list to pick a victim.  Keeping it here rather than wiring generators to
each other keeps every generator independent of the others' presence.
"""

from __future__ import annotations

import random
import threading
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple


@dataclass(frozen=True)
class CreatedObject:
    path: str
    path_id: Optional[int]
    schemeshard_id: Optional[int]
    tablet_ids: Tuple[int, ...] = ()


class LiveObjects:
    """Thread-safe registry of objects the workload created and not yet dropped."""

    def __init__(self):
        self._lock = threading.Lock()
        self._objects: Dict[str, CreatedObject] = {}

    def add(self, obj: CreatedObject) -> None:
        with self._lock:
            self._objects[obj.path] = obj

    def remove(self, path: str) -> None:
        with self._lock:
            self._objects.pop(path, None)

    def count(self) -> int:
        with self._lock:
            return len(self._objects)

    def paths(self) -> List[str]:
        with self._lock:
            return list(self._objects)

    def take_random(self) -> Optional[CreatedObject]:
        """Remove and return a random object, so two droppers never race on one."""
        with self._lock:
            if not self._objects:
                return None
            path = random.choice(list(self._objects))
            return self._objects.pop(path)

    def random_tablet(self) -> Optional[int]:
        with self._lock:
            candidates = [obj for obj in self._objects.values() if obj.tablet_ids]
            if not candidates:
                return None
            return random.choice(random.choice(candidates).tablet_ids)
