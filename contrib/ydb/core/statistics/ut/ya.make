UNITTEST_FOR(contrib/ydb/core/statistics)

FORK_SUBTESTS()

IF (WITH_VALGRIND)
    TIMEOUT(3600)
    SIZE(LARGE)
    TAG(ya:fat)
ELSE()
    TIMEOUT(600)
    SIZE(MEDIUM)
ENDIF()

YQL_LAST_ABI_VERSION()

PEERDIR(
    library/cpp/testing/unittest
    contrib/ydb/core/testlib/default
)

SRCS(
    ut_common.h
    ut_common.cpp
    ut_statistics.cpp
)

END()
