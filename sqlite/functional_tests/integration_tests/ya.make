PROGRAM(userver-functional-test-service)

ALLOCATOR(J)

SUBSCRIBER(g:taxi-common)

PEERDIR(
    taxi/uservices/userver/core
    taxi/uservices/userver/sqlite
)

ADDINCL(
    taxi/uservices/userver/sqlite/functional_tests/integration_tests/src
)

SRCS(
    service.cpp
)

USRV_ALL_SRCS(
    RECURSIVE src
)

END()

RECURSE_FOR_TESTS(tests)
