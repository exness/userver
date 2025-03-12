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
    """

    ERROR_CODE = 'timeout'


class NetworkError(MockedError):
    """
    @brief When thrown from a mocked handler, leads to `ugrpc::client::RpcInterruptedError`.
    @alias pytest_userver.grpc.NetworkError
    """

    ERROR_CODE = 'network'
