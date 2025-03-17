import asyncio
import fcntl
import os
import socket
import time
import typing
import uuid

import pytest
from pytest_userver import chaos  # pylint: disable=import-error

_NOTICEABLE_DELAY = 0.5
TEST_TIMEOUT = 5
RECV_MAX_SIZE = 4096


# uvloop has no implementation for these methods
async def uvloop_recvfrom(sock: socket.socket, max_size: int, loop):
    fut = loop.create_future()

    def _cb():
        try:
            ret = sock.recvfrom(max_size)
        except (BlockingIOError, InterruptedError):
            pass
        except (  # pylint: disable=try-except-raise
            KeyboardInterrupt,
            SystemExit,
        ):
            raise
        except Exception as exc:  # pylint: disable=broad-exception-caught
            fut.set_exception(exc)
            loop.remove_reader(sock)
        else:
            fut.set_result(ret)
            loop.remove_reader(sock)

    loop.add_reader(sock, _cb)
    return await fut


async def uvloop_sendto(sock: socket.socket, data: bytes, addr, loop):
    fut = loop.create_future()

    def _cb():
        try:
            ret = sock.sendto(data, addr)
        except (BlockingIOError, InterruptedError):
            pass
        except (  # pylint: disable=try-except-raise
            KeyboardInterrupt,
            SystemExit,
        ):
            raise
        except Exception as exc:  # pylint: disable=broad-exception-caught
            fut.set_exception(exc)
            loop.remove_writer(sock)
        else:
            fut.set_result(ret)
            loop.remove_writer(sock)

    loop.add_writer(sock, _cb)
    return await fut


def _has_data(recv_socket: socket.socket) -> bool:
    try:
        data = recv_socket.recv(1, socket.MSG_PEEK)
        return bool(data)
    except (BlockingIOError, InterruptedError):
        return False


def _make_socket() -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    fcntl.fcntl(sock, fcntl.F_SETFL, os.O_NONBLOCK)
    return sock


async def _assert_receive_timeout(sock: socket.socket) -> None:
    loop = asyncio.get_running_loop()
    try:
        data = await asyncio.wait_for(
            loop.sock_recv(sock, 1),
            _NOTICEABLE_DELAY,
        )
        assert not data
    except asyncio.TimeoutError:
        pass


class UdpServer:
    def __init__(self, sock: socket.socket):
        self._sock = sock
        self._loop = asyncio.get_running_loop()

    @property
    def loop(self):
        return self._loop

    @property
    def sock(self):
        return self._sock

    @property
    def port(self):
        return self._sock.getsockname()[1]

    async def resolve(self, client_sock: socket.socket):
        """
        Returns an address of the client as seen by the server
        """
        await self._loop.sock_sendall(client_sock, b'ping')
        msg, addr = await chaos._get_message_task(  # pylint: disable=protected-access
            self._sock,
        )
        assert msg == b'ping'
        return addr


@pytest.fixture(name='udp_server', scope='function')
async def _server():
    sock = _make_socket()
    sock.bind(('127.0.0.1', 0))
    try:
        yield UdpServer(sock)
    finally:
        sock.close()


@pytest.fixture(name='gate', scope='function')
async def _gate(udp_server):
    gate_config = chaos.GateRoute(
        name='udp proxy',
        host_to_server='127.0.0.1',
        port_to_server=udp_server.port,
    )
    async with chaos.UdpGate(gate_config) as proxy:
        yield proxy


class Client(typing.NamedTuple):
    sock: socket.socket
    addr_at_server: typing.Any


@pytest.fixture(name='udp_client_factory', scope='function')
async def _client_factory(gate, udp_server: UdpServer):
    client_sockets: typing.List[socket.socket] = []

    loop = udp_server.loop

    async def _client_factory_impl():
        sock = _make_socket()
        await loop.sock_connect(sock, gate.get_sockname_for_clients())
        client_sockets.append(sock)
        addr_at_server = await udp_server.resolve(sock)
        return Client(sock, addr_at_server)

    yield _client_factory_impl

    for sock in client_sockets:
        sock.close()


async def _assert_data_from_client(client: Client, server: UdpServer):
    loop = server.loop
    expected = b'ping_' + uuid.uuid4().bytes
    await loop.sock_sendall(client.sock, expected)
    data, addr = await uvloop_recvfrom(server.sock, len(expected), loop)
    assert data == expected
    assert addr == client.addr_at_server


async def _assert_data_to_client(server: UdpServer, client: Client):
    loop = server.loop
    expected = b'pong_' + uuid.uuid4().bytes
    await uvloop_sendto(server.sock, expected, client.addr_at_server, loop)
    data = await loop.sock_recv(client.sock, len(expected))
    assert data == expected


async def test_basic(udp_server, udp_client_factory, gate):
    udp_client = await udp_client_factory()

    await _assert_data_from_client(udp_client, udp_server)
    await _assert_data_to_client(udp_server, udp_client)

    assert gate.is_connected()
    await gate.sockets_close()
    assert not gate.is_connected()


async def test_to_client_udp_message_queue(
    udp_server,
    udp_client_factory,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    messages_count = 5
    for i in range(messages_count):
        await uvloop_sendto(
            udp_server.sock,
            b'message' + i.to_bytes(1, 'big'),
            udp_client.addr_at_server,
            asyncio_loop,
        )

    for i in range(messages_count):
        assert await asyncio_loop.sock_recv(
            udp_client.sock,
            RECV_MAX_SIZE,
        ) == b'message' + i.to_bytes(1, 'big')


async def test_to_server_udp_message_queue(
    udp_server,
    udp_client_factory,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    messages_count = 5
    for i in range(messages_count):
        await asyncio_loop.sock_sendall(
            udp_client.sock,
            b'message' + i.to_bytes(1, 'big'),
        )

    for i in range(messages_count):
        assert await asyncio_loop.sock_recv(
            udp_server.sock,
            RECV_MAX_SIZE,
        ) == b'message' + i.to_bytes(1, 'big')


async def test_fifo_udp_message_queue(udp_server, udp_client_factory, asyncio_loop):
    udp_client = await udp_client_factory()

    messages_count = 5
    for i in range(messages_count):
        await asyncio_loop.sock_sendall(
            udp_client.sock,
            b'message' + i.to_bytes(1, 'big'),
        )

    for i in range(messages_count):
        await uvloop_sendto(
            udp_server.sock,
            b'message' + i.to_bytes(1, 'big'),
            udp_client.addr_at_server,
            asyncio_loop,
        )

    for i in range(messages_count):
        assert await asyncio_loop.sock_recv(
            udp_client.sock,
            RECV_MAX_SIZE,
        ) == b'message' + i.to_bytes(1, 'big')

    for i in range(messages_count):
        assert await asyncio_loop.sock_recv(
            udp_server.sock,
            RECV_MAX_SIZE,
        ) == b'message' + i.to_bytes(1, 'big')


async def test_to_server_parallel_udp_message(
    udp_server,
    udp_client_factory,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    messages_count = 100

    tasks: typing.List[typing.Awaitable] = []
    for i in range(messages_count):
        tasks.append(
            asyncio_loop.sock_sendall(
                udp_client.sock,
                b'message' + i.to_bytes(1, 'big'),
            ),
        )

    await asyncio.gather(*tasks)

    res: typing.List[bool] = [False] * messages_count
    for _ in range(messages_count):
        message = await asyncio_loop.sock_recv(udp_server.sock, RECV_MAX_SIZE)
        assert message[:7] == b'message'
        idx = int.from_bytes(message[7:], 'big')
        res[idx] = True

    assert all(res)


async def test_to_client_noop(udp_server, udp_client_factory, gate, asyncio_loop):
    udp_client = await udp_client_factory()

    gate.to_client_noop()

    await uvloop_sendto(
        udp_server.sock,
        b'ping',
        udp_client.addr_at_server,
        asyncio_loop,
    )
    await _assert_data_from_client(udp_client, udp_server)
    assert not _has_data(udp_client.sock)

    gate.to_client_pass()
    hello = await asyncio_loop.sock_recv(udp_client.sock, 4)
    assert hello == b'ping'
    await _assert_data_to_client(udp_server, udp_client)
    await _assert_data_from_client(udp_client, udp_server)


async def test_to_server_noop(udp_server, udp_client_factory, gate, asyncio_loop):
    udp_client = await udp_client_factory()

    gate.to_server_noop()

    await asyncio_loop.sock_sendall(udp_client.sock, b'ping')
    await _assert_data_to_client(udp_server, udp_client)
    assert not _has_data(udp_server.sock)

    gate.to_server_pass()
    server_incoming_data = await asyncio_loop.sock_recv(udp_server.sock, 4)
    assert server_incoming_data == b'ping'
    await _assert_data_to_client(udp_server, udp_client)
    await _assert_data_from_client(udp_client, udp_server)


async def test_to_client_drop(udp_server, udp_client_factory, gate, asyncio_loop):
    udp_client = await udp_client_factory()

    gate.to_client_drop()

    await uvloop_sendto(
        udp_server.sock,
        b'ping',
        udp_client.addr_at_server,
        asyncio_loop,
    )
    await _assert_data_from_client(udp_client, udp_server)
    assert not _has_data(udp_client.sock)

    gate.to_client_pass()
    await _assert_receive_timeout(udp_client.sock)


async def test_to_server_drop(udp_server, udp_client_factory, gate, asyncio_loop):
    udp_client = await udp_client_factory()

    gate.to_server_drop()

    await asyncio_loop.sock_sendall(udp_client.sock, b'ping')
    await _assert_data_to_client(udp_server, udp_client)
    assert not _has_data(udp_server.sock)

    gate.to_server_pass()
    await _assert_receive_timeout(udp_server.sock)


async def test_to_client_delay(udp_server, udp_client_factory, gate):
    udp_client = await udp_client_factory()

    gate.to_client_delay(2 * _NOTICEABLE_DELAY)

    await _assert_data_from_client(udp_client, udp_server)

    start_time = time.monotonic()
    await _assert_data_to_client(udp_server, udp_client)
    assert time.monotonic() - start_time >= _NOTICEABLE_DELAY


async def test_to_server_delay(udp_server, udp_client_factory, gate):
    udp_client = await udp_client_factory()

    gate.to_server_delay(2 * _NOTICEABLE_DELAY)

    await _assert_data_to_client(udp_server, udp_client)

    start_time = time.monotonic()
    await _assert_data_from_client(udp_client, udp_server)
    assert time.monotonic() - start_time >= _NOTICEABLE_DELAY


async def test_to_client_close_on_data(
    udp_server,
    udp_client_factory,
    gate,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    gate.to_client_close_on_data()

    await _assert_data_from_client(udp_client, udp_server)
    assert gate.is_connected()
    await uvloop_sendto(
        udp_server.sock,
        b'die',
        udp_client.addr_at_server,
        asyncio_loop,
    )

    await _assert_receive_timeout(udp_client.sock)


async def test_to_server_close_on_data(
    udp_server,
    udp_client_factory,
    gate,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    gate.to_server_close_on_data()

    await _assert_data_to_client(udp_server, udp_client)
    assert gate.is_connected()
    await asyncio_loop.sock_sendall(udp_client.sock, b'die')

    await _assert_receive_timeout(udp_server.sock)


async def test_to_client_corrupt_data(
    udp_server,
    udp_client_factory,
    gate,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    gate.to_client_corrupt_data()

    await _assert_data_from_client(udp_client, udp_server)
    assert gate.is_connected()

    await uvloop_sendto(
        udp_server.sock,
        b'break me',
        udp_client.addr_at_server,
        asyncio_loop,
    )
    data = await asyncio_loop.sock_recv(udp_client.sock, 512)
    assert data
    assert data != b'break me'

    gate.to_client_pass()
    await _assert_data_to_client(udp_server, udp_client)
    await _assert_data_from_client(udp_client, udp_server)


async def test_to_server_corrupt_data(
    udp_server,
    udp_client_factory,
    gate,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    gate.to_server_corrupt_data()

    await _assert_data_to_client(udp_server, udp_client)
    assert gate.is_connected()

    await asyncio_loop.sock_sendall(udp_client.sock, b'break me')
    data = await asyncio_loop.sock_recv(udp_server.sock, 512)
    assert data
    assert data != b'break me'

    gate.to_server_pass()
    await _assert_data_to_client(udp_server, udp_client)
    await _assert_data_from_client(udp_client, udp_server)


async def test_to_client_limit_bps(udp_server, udp_client_factory, gate, asyncio_loop):
    udp_client = await udp_client_factory()

    gate.to_client_limit_bps(2)

    start_time = time.monotonic()

    message = b'hello'

    # udp sockets drops message ending, so we have only the part what
    # was read on first try
    for _ in range(5):
        await uvloop_sendto(
            udp_server.sock,
            message,
            udp_client.addr_at_server,
            asyncio_loop,
        )
        data = await asyncio_loop.sock_recv(udp_client.sock, 5)
        assert data
        assert data != message and message.startswith(data)

    assert time.monotonic() - start_time >= 1.0


async def test_to_server_limit_bps(udp_server, udp_client_factory, gate, asyncio_loop):
    udp_client = await udp_client_factory()

    gate.to_server_limit_bps(2)

    start_time = time.monotonic()

    message = b'hello'

    # udp sockets drops message ending, so we have only the
    # part what was read on first try
    for _ in range(5):
        await asyncio_loop.sock_sendall(udp_client.sock, b'hello')
        data = await asyncio_loop.sock_recv(udp_server.sock, 5)
        assert data
        assert len(data) and message.startswith(data)

    assert time.monotonic() - start_time >= 1.0


async def test_to_client_limit_time(
    udp_server,
    udp_client_factory,
    gate,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    gate.to_client_limit_time(_NOTICEABLE_DELAY, jitter=0.0)

    await _assert_data_from_client(udp_client, udp_server)
    assert gate.is_connected()

    await asyncio.sleep(_NOTICEABLE_DELAY)

    await _assert_data_to_client(udp_server, udp_client)
    await asyncio.sleep(_NOTICEABLE_DELAY * 2)
    await uvloop_sendto(
        udp_server.sock,
        b'die',
        udp_client.addr_at_server,
        asyncio_loop,
    )
    await _assert_receive_timeout(udp_client.sock)


async def test_to_server_limit_time(
    udp_server,
    udp_client_factory,
    gate,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    gate.to_server_limit_time(_NOTICEABLE_DELAY, jitter=0.0)

    await _assert_data_to_client(udp_server, udp_client)
    assert gate.is_connected()

    await asyncio.sleep(_NOTICEABLE_DELAY)

    await _assert_data_from_client(udp_client, udp_server)
    await asyncio.sleep(_NOTICEABLE_DELAY * 2)
    await asyncio_loop.sock_sendall(udp_client.sock, b'die')
    await _assert_receive_timeout(udp_server.sock)


async def test_to_client_limit_bytes(
    udp_server,
    udp_client_factory,
    gate,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    gate.to_client_limit_bytes(14)

    await _assert_data_from_client(udp_client, udp_server)
    assert gate.is_connected()

    await uvloop_sendto(
        udp_server.sock,
        b'hello',
        udp_client.addr_at_server,
        asyncio_loop,
    )
    data = await asyncio_loop.sock_recv(udp_client.sock, 10)
    assert data == b'hello'

    await uvloop_sendto(
        udp_server.sock,
        b'die after',
        udp_client.addr_at_server,
        asyncio_loop,
    )
    data = await asyncio_loop.sock_recv(udp_client.sock, 10)
    assert data == b'die after'

    await uvloop_sendto(
        udp_server.sock,
        b'dead now',
        udp_client.addr_at_server,
        asyncio_loop,
    )

    await _assert_receive_timeout(udp_client.sock)


async def test_to_server_limit_bytes(
    udp_server,
    udp_client_factory,
    gate,
    asyncio_loop,
):
    udp_client = await udp_client_factory()

    gate.to_server_limit_bytes(14)

    await _assert_data_to_client(udp_server, udp_client)
    assert gate.is_connected()

    await asyncio_loop.sock_sendall(udp_client.sock, b'hello')
    data = await asyncio_loop.sock_recv(udp_server.sock, 10)
    assert data == b'hello'

    await asyncio_loop.sock_sendall(udp_client.sock, b'die after')
    data = await asyncio_loop.sock_recv(udp_server.sock, 10)
    assert data == b'die after'

    await _assert_receive_timeout(udp_server.sock)


async def test_substitute(udp_server, udp_client_factory, gate, asyncio_loop):
    udp_client = await udp_client_factory()

    gate.to_server_substitute('hello', 'die')
    gate.to_client_substitute('hello', 'die')

    await _assert_data_to_client(udp_server, udp_client)
    await _assert_data_from_client(udp_client, udp_server)

    await asyncio_loop.sock_sendall(udp_client.sock, b'hello')
    data = await asyncio_loop.sock_recv(udp_server.sock, 10)
    assert data == b'die'

    await uvloop_sendto(
        udp_server.sock,
        b'hello',
        udp_client.addr_at_server,
        asyncio_loop,
    )
    data = await asyncio_loop.sock_recv(udp_client.sock, 10)
    assert data == b'die'


async def test_wait_for_connections(udp_client_factory, gate):
    await udp_client_factory()
    assert gate.is_connected()
    with pytest.raises(AttributeError):
        await gate.wait_for_connections(count=1, timeout=_NOTICEABLE_DELAY)


async def test_start_stop(udp_server, udp_client_factory, gate, asyncio_loop):
    udp_client = await udp_client_factory()

    await gate.stop()

    await asyncio_loop.sock_sendall(udp_client.sock, b'hello')
    await _assert_receive_timeout(udp_server.sock)

    gate.start()
    udp_client2 = await udp_client_factory()

    await _assert_data_to_client(udp_server, udp_client2)
    await _assert_data_from_client(udp_client2, udp_server)


async def test_multi_client(udp_server, udp_client_factory, gate):
    udp_client = await udp_client_factory()
    udp_client2 = await udp_client_factory()

    await _assert_data_to_client(udp_server, udp_client)
    await _assert_data_to_client(udp_server, udp_client2)
    await _assert_data_from_client(udp_client2, udp_server)
    await _assert_data_from_client(udp_client, udp_server)
