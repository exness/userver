"""
Mocks for the gRPC servers.

@sa @ref scripts/docs/en/userver/tutorial/grpc_service.md
@sa @ref pytest_userver.plugins.grpc.mockserver
"""

import grpc  # noqa # pylint: disable=unused-import


class MockedError(Exception):
    """
    @brief Base class for mocked exceptions.
    @alias pytest_userver.grpc.MockedError
    """

    ERROR_CODE = 'unknown'


class TimeoutError(MockedError):  # pylint: disable=redefined-builtin
    """
    @brief When thrown from a mocked handler, leads to `ugrpc::client::RpcInterruptedError`.
    @alias pytest_userver.grpc.TimeoutError

    Example:
    @snippet grpc/functional_tests/middleware_client/tests/test_mocked_fail.py Mocked timeout failure
    """

    ERROR_CODE = 'timeout'


class NetworkError(MockedError):
    """
    @brief When thrown from a mocked handler, leads to `ugrpc::client::RpcInterruptedError`.
    @alias pytest_userver.grpc.NetworkError

    Example:
    @snippet grpc/functional_tests/middleware_client/tests/test_mocked_fail.py Mocked network failure
    """

    ERROR_CODE = 'network'
