PY3_LIBRARY()

PY_SRCS(
    __init__.py
    ledger.py
    registry.py
    shared.py
    generators/__init__.py
    generators/bsc_config.py
    generators/ddl_churn.py
    generators/register_node.py
    generators/restart_churn.py
)

PEERDIR(
    contrib/python/PyYAML
    contrib/python/requests
    ydb/tests/library
    ydb/tests/library/clients
    ydb/tests/stress/common
    ydb/public/sdk/python
    ydb/public/sdk/python/enable_v3_new_behavior
)

END()
