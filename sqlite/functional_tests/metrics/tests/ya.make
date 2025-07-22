SUBSCRIBER(g:taxi-common)

PY3TEST()

ALL_PYTEST_SRCS()

DEPENDS(
    taxi/uservices/userver/sqlite/functional_tests/metrics
)

DATA(
    arcadia/taxi/uservices/userver/sqlite/functional_tests/metrics
)

PEERDIR(
    taxi/uservices/testsuite/integration_testing
    taxi/uservices/userver/testsuite/pytest_plugins/pytest_userver
    taxi/uservices/userver-arc-utils/functional_tests/pytest_plugins
)

CONFTEST_LOAD_POLICY_LOCAL()
TEST_CWD(/)

END()
