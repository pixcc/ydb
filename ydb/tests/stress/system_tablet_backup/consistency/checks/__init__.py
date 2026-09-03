# -*- coding: utf-8 -*-
"""The invariants.

Importing this package registers every check.  To add a module, drop the file
next to these and append its name to ``_MODULES`` -- explicit imports keep the
package working both as a plain directory on a production host and as an
Arcadia PY3_LIBRARY, where filesystem-based discovery is not available.
"""

from importlib import import_module

_MODULES = (
    "refs",
    "sequences",
    "storage",
    "tenants",
    "replay",
    "ledger_checks",
    "meta",
)

for _name in _MODULES:
    import_module("." + _name, __name__)

del import_module, _name
