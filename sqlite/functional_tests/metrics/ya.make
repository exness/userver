PROGRAM(userver-functional-test-service)

ALLOCATOR(J)

SUBSCRIBER(g:taxi-common)

PEERDIR(
    taxi/uservices/userver/core
    taxi/uservices/userver/sqlite
)

SRCS(
    service.cpp
)

END()

RECURSE_FOR_TESTS(tests)
