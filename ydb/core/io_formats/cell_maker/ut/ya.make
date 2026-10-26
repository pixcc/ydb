UNITTEST_FOR(ydb/core/io_formats/cell_maker)
SRCS(cell_maker_ut.cpp)
PEERDIR(
    ydb/core/testlib/pg
    yql/essentials/public/udf/service/exception_policy
)
YQL_LAST_ABI_VERSION()
END()
