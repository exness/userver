# /// [Functional test]
import socket

import pytest


async def test_basic(service_client, asyncio_loop, tcp_service_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('localhost', tcp_service_port))

    await asyncio_loop.sock_sendall(sock, b'hi')
    hello = await asyncio_loop.sock_recv(sock, 5)
    assert hello == b'hello'

    await asyncio_loop.sock_sendall(sock, b'whats up?')
    with pytest.raises(ConnectionResetError):
        await asyncio_loop.sock_recv(sock, 1)
    # /// [Functional test]
