PY3TEST()

TEST_SRCS(
    test_consistency.py
)

PY_SRCS(
    fake_backup.py
)

PEERDIR(
    ydb/tests/stress/system_tablet_backup/consistency
)

SIZE(SMALL)

END()
