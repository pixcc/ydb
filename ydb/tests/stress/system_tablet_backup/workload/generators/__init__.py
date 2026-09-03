# -*- coding: utf-8 -*-
"""Load generators.

Importing this package registers every generator.  To add one, drop the module
next to these and append its name to ``_MODULES``.

Generator names map to the plan:

    ddl_churn       G1  SchemeShard -> Hive -> BSController reference churn
    restart_churn   G2  Hive TxStartTablet / KnownGeneration churn
    bsc_config      G4  BSController TTxConfigCmd churn
    register_node   G5  NodeBroker load

G3 (metric pressure on Hive) is deliberately absent: the send period is a
compile-time constant, so the only lever is the number of live tablets, which
``ddl_churn`` already controls through its partition count.
"""

from importlib import import_module

_MODULES = (
    "ddl_churn",
    "restart_churn",
    "bsc_config",
    "register_node",
)

for _name in _MODULES:
    import_module("." + _name, __name__)

del import_module, _name
