GTEST()

ALLOCATOR(J)

SUBSCRIBER(
    g:taxi-common
)

SRCDIR(taxi/uservices/userver/sqlite)

CFLAGS(
    -Wno-unused-parameter
    -Wno-inconsistent-missing-override
)

PEERDIR(
    taxi/uservices/userver-arc-utils/testing
    taxi/uservices/userver/core/testing
    taxi/uservices/userver/sqlite
)

ADDINCL(
    contrib/libs/sqlite3
    taxi/uservices/userver/core/src
    taxi/uservices/userver/sqlite/src
)

USRV_ALL_SRCS(
    RECURSIVE ../src
    SUFFIX test
)

FORK_SUBTESTS()
SPLIT_FACTOR(5)

END()
