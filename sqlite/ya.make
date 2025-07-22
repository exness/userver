LIBRARY()

SUBSCRIBER(
    g:taxi-common
)

INCLUDE(${ARCADIA_ROOT}/taxi/uservices/userver/sqlite/ya.make.ext)

CFLAGS(
    -DSQLITE_THREADSAFE=2
)

PEERDIR(
    contrib/libs/sqlite3
    taxi/uservices/userver/core
)

ADDINCL(
    GLOBAL taxi/uservices/userver/sqlite/include
    contrib/libs/sqlite3
    taxi/uservices/userver/sqlite/src
)

USRV_ALL_SRCS(
    RECURSIVE src
    EXCLUDE **/*_benchmark.* **/*test.*
)

END()

RECURSE_FOR_TESTS(
    functional_tests
    tests
)
