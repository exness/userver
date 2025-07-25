# pylint: disable=too-many-lines
"""
Python module that provides testsuite support for
chaos tests; see
@ref scripts/docs/en/userver/chaos_testing.md for an introduction.

@ingroup userver_testsuite
"""

import asyncio
import dataclasses
import functools
import io
import logging
import random
import re
import socket
import time
import typing

import pytest

from testsuite import asyncio_socket
from testsuite.utils import callinfo


class BaseError(Exception):
    pass


class ConnectionClosedError(BaseError):
    pass


@dataclasses.dataclass(frozen=True)
class GateRoute:
    """
    Class that describes the route for TcpGate or UdpGate.

    Use `port_for_client == 0` to bind to some unused port. In that case the
    actual address could be retrieved via BaseGate.get_sockname_for_clients().

    @ingroup userver_testsuite
    """

    name: str
    host_to_server: str
    port_to_server: int
    host_for_client: str = '127.0.0.1'
    port_for_client: int = 0


# @cond

# https://docs.python.org/3/library/socket.html#socket.socket.recv
RECV_MAX_SIZE = 4096
MAX_DELAY = 60.0


logger = logging.getLogger(__name__)


Address = typing.Tuple[str, int]
EvLoop = typing.Any
Socket = socket.socket
Interceptor = typing.Callable[
    [EvLoop, Socket, Socket],
    typing.Coroutine[typing.Any, typing.Any, None],
]


class GateException(Exception):
    pass


class GateInterceptException(Exception):
    pass


async def _intercept_ok(
    loop: EvLoop,
    socket_from: Socket,
    socket_to: Socket,
) -> None:
    data = await loop.sock_recv(socket_from, RECV_MAX_SIZE)
    if not data:
        raise ConnectionClosedError()
    await loop.sock_sendall(socket_to, data)


async def _intercept_drop(
    loop: EvLoop,
    socket_from: Socket,
    socket_to: Socket,
) -> None:
    data = await loop.sock_recv(socket_from, RECV_MAX_SIZE)
    if not data:
        raise ConnectionClosedError()


async def _intercept_delay(
    delay: float,
    loop: EvLoop,
    socket_from: Socket,
    socket_to: Socket,
) -> None:
    data = await loop.sock_recv(socket_from, RECV_MAX_SIZE)
    if not data:
        raise ConnectionClosedError()
    await asyncio.sleep(delay)
    await loop.sock_sendall(socket_to, data)


async def _intercept_close_on_data(
    loop: EvLoop,
    socket_from: Socket,
    socket_to: Socket,
) -> None:
    data = await loop.sock_recv(socket_from, 1)
    if not data:
        raise ConnectionClosedError()
    raise GateInterceptException('Closing socket on data')


async def _intercept_corrupt(
    loop: EvLoop,
    socket_from: Socket,
    socket_to: Socket,
) -> None:
    data = await loop.sock_recv(socket_from, RECV_MAX_SIZE)
    if not data:
        raise ConnectionClosedError()
    await loop.sock_sendall(socket_to, bytearray([not x for x in data]))


class _InterceptBpsLimit:
    def __init__(self, bytes_per_second: float):
        assert bytes_per_second >= 1
        self._bytes_per_second = bytes_per_second
        self._time_last_added = 0.0
        self._bytes_left = self._bytes_per_second

    def _update_limit(self) -> None:
        current_time = time.monotonic()
        elapsed = current_time - self._time_last_added
        bytes_addition = self._bytes_per_second * elapsed
        if bytes_addition > 0:
            self._bytes_left += bytes_addition
            self._time_last_added = current_time

            if self._bytes_left > self._bytes_per_second:
                self._bytes_left = self._bytes_per_second

    async def __call__(
        self,
        loop: EvLoop,
        socket_from: Socket,
        socket_to: Socket,
    ) -> None:
        self._update_limit()

        bytes_to_recv = min(int(self._bytes_left), RECV_MAX_SIZE)
        if bytes_to_recv > 0:
            data = await loop.sock_recv(socket_from, bytes_to_recv)
            if not data:
                raise ConnectionClosedError()
            self._bytes_left -= len(data)

            await loop.sock_sendall(socket_to, data)
        else:
            logger.info('Socket hits the bytes per second limit')
            await asyncio.sleep(1.0 / self._bytes_per_second)


class _InterceptTimeLimit:
    def __init__(self, timeout: float, jitter: float):
        self._sockets: typing.Dict[Socket, float] = {}
        assert timeout >= 0.0
        self._timeout = timeout
        assert jitter >= 0.0
        self._jitter = jitter

    def raise_if_timed_out(self, socket_from: Socket) -> None:
        if socket_from not in self._sockets:
            jitter = self._jitter * random.random()
            expire_at = time.monotonic() + self._timeout + jitter
            self._sockets[socket_from] = expire_at

        if self._sockets[socket_from] <= time.monotonic():
            del self._sockets[socket_from]
            raise GateInterceptException('Socket hits the time limit')

    async def __call__(
        self,
        loop: EvLoop,
        socket_from: Socket,
        socket_to: Socket,
    ) -> None:
        self.raise_if_timed_out(socket_from)
        await _intercept_ok(loop, socket_from, socket_to)


class _InterceptSmallerParts:
    def __init__(self, max_size: int, sleep_per_packet: float):
        assert max_size > 0
        self._max_size = max_size
        self._sleep_per_packet = sleep_per_packet

    async def __call__(
        self,
        loop: EvLoop,
        socket_from: Socket,
        socket_to: Socket,
    ) -> None:
        data = await loop.sock_recv(socket_from, self._max_size)
        if not data:
            raise ConnectionClosedError()
        await asyncio.sleep(self._sleep_per_packet)
        await loop.sock_sendall(socket_to, data)


class _InterceptConcatPackets:
    def __init__(self, packet_size: int):
        assert packet_size >= 0
        self._packet_size = packet_size
        self._expire_at: typing.Optional[float] = None
        self._buf = io.BytesIO()

    async def __call__(
        self,
        loop: EvLoop,
        socket_from: Socket,
        socket_to: Socket,
    ) -> None:
        if self._expire_at is None:
            self._expire_at = time.monotonic() + MAX_DELAY

        if self._expire_at <= time.monotonic():
            pytest.fail(
                f'Failed to make a packet of sufficient size in {MAX_DELAY} '
                'seconds. Check the test logic, it should end with checking '
                'that the data was sent and by calling TcpGate function '
                'to_client_pass() to pass the remaining packets.',
            )

        data = await loop.sock_recv(socket_from, RECV_MAX_SIZE)
        if not data:
            raise ConnectionClosedError()
        self._buf.write(data)
        if self._buf.tell() >= self._packet_size:
            await loop.sock_sendall(socket_to, self._buf.getvalue())
            self._buf = io.BytesIO()
            self._expire_at = None


class _InterceptBytesLimit:
    def __init__(self, bytes_limit: int, gate: 'BaseGate'):
        assert bytes_limit >= 0
        self._bytes_limit = bytes_limit
        self._bytes_remain = self._bytes_limit
        self._gate = gate

    async def __call__(
        self,
        loop: EvLoop,
        socket_from: Socket,
        socket_to: Socket,
    ) -> None:
        data = await loop.sock_recv(socket_from, RECV_MAX_SIZE)
        if not data:
            raise ConnectionClosedError()
        if self._bytes_remain <= len(data):
            await loop.sock_sendall(socket_to, data[0 : self._bytes_remain])
            await self._gate.sockets_close()
            self._bytes_remain = self._bytes_limit
            raise GateInterceptException('Data transmission limit reached')
        self._bytes_remain -= len(data)
        await loop.sock_sendall(socket_to, data)


class _InterceptSubstitute:
    def __init__(self, pattern: str, repl: str, encoding='utf-8'):
        self._pattern = re.compile(pattern)
        self._repl = repl
        self._encoding = encoding

    async def __call__(
        self,
        loop: EvLoop,
        socket_from: Socket,
        socket_to: Socket,
    ) -> None:
        data = await loop.sock_recv(socket_from, RECV_MAX_SIZE)
        if not data:
            raise ConnectionClosedError()
        try:
            res = self._pattern.sub(self._repl, data.decode(self._encoding))
            data = res.encode(self._encoding)
        except UnicodeError:
            pass
        await loop.sock_sendall(socket_to, data)


async def _cancel_and_join(task: typing.Optional[asyncio.Task]) -> None:
    if not task or task.cancelled():
        return

    try:
        task.cancel()
        await task
    except asyncio.CancelledError:
        return
    except Exception:  # pylint: disable=broad-except
        logger.exception('Exception in _cancel_and_join')


def _make_socket_nonblocking(sock: Socket) -> None:
    sock.setblocking(False)
    if sock.type == socket.SOCK_STREAM:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


class _UdpDemuxSocketMock:
    """
    Emulates a point-to-point connection over UDP socket
    with a non-blocking socket interface
    """

    def gettimeout(self):
        return self._sock.gettimeout()

    def __init__(self, sock: Socket, peer_address: Address):
        self._sock: Socket = sock
        self._peeraddr: Address = peer_address

        sockpair = socket.socketpair(type=socket.SOCK_DGRAM)
        self._demux_in: Socket = sockpair[0]
        self._demux_out: Socket = sockpair[1]
        _make_socket_nonblocking(self._demux_in)
        _make_socket_nonblocking(self._demux_out)
        self._is_active: bool = True

    @property
    def peer_address(self):
        return self._peeraddr

    async def push(self, loop: EvLoop, data: bytes):
        return await loop.sock_sendall(self._demux_in, data)

    def is_active(self):
        return self._is_active

    def close(self):
        self._is_active = False
        self._demux_out.close()
        self._demux_in.close()

    def recvfrom(self, bufsize: int, flags: int = 0):
        return self._demux_out.recvfrom(bufsize, flags)

    def recv(self, bufsize: int, flags: int = 0):
        return self._demux_out.recv(bufsize, flags)

    def get_demux_out(self):
        return self._demux_out

    def fileno(self):
        return self._demux_out.fileno()

    def send(self, data: bytes):
        return self._sock.sendto(data, self._peeraddr)


class InterceptTask:
    def __init__(self, socket_from, socket_to, interceptor):
        self._socket_from = socket_from
        self._socket_to = socket_to
        self._condition = asyncio.Condition()
        self._interceptor = interceptor

    def get_interceptor(self):
        return self._interceptor

    async def set_interceptor(self, interceptor):
        async with self._condition:
            self._interceptor = interceptor
            self._condition.notify()

    async def run(self):
        loop = asyncio.get_running_loop()
        while True:
            # Applies new interceptors faster.
            #
            # To avoid long awaiting on sock_recv in an outdated
            # interceptor we wait for data before grabbing and applying
            # the interceptor.
            await _wait_for_data(self._socket_from)

            # Wait for interceptor attached
            async with self._condition:
                interceptor = await self._condition.wait_for(self.get_interceptor)

            logging.trace('running interceptor: %s', interceptor)
            await interceptor(loop, self._socket_from, self._socket_to)


class _SocketsPaired:
    def __init__(
        self,
        proxy_name: str,
        loop: EvLoop,
        client: typing.Union[socket.socket, _UdpDemuxSocketMock],
        server: socket.socket,
        to_server_intercept: Interceptor,
        to_client_intercept: Interceptor,
    ) -> None:
        self._proxy_name = proxy_name

        self._client = client
        self._server = server

        self._task_to_server = InterceptTask(client, server, to_server_intercept)
        self._task_to_client = InterceptTask(server, client, to_client_intercept)

        self._task = asyncio.create_task(self._run())
        self._interceptor_tasks = []

    async def set_to_server_interceptor(self, interceptor):
        await self._task_to_server.set_interceptor(interceptor)

    async def set_to_client_interceptor(self, interceptor: Interceptor):
        await self._task_to_client.set_interceptor(interceptor)

    async def shutdown(self) -> None:
        for task in self._interceptor_tasks:
            await _cancel_and_join(task)
        await _cancel_and_join(self._task)

    def is_active(self) -> bool:
        return not self._task.done()

    def info(self) -> str:
        if not self.is_active():
            return '<inactive>'

        return f'client fd={self._client.fileno()} <=> server fd={self._server.fileno()}'

    async def _run(self):
        self._interceptor_tasks = [
            asyncio.create_task(obj.run()) for obj in (self._task_to_server, self._task_to_client)
        ]
        try:
            done, _ = await asyncio.wait(self._interceptor_tasks, return_when=asyncio.FIRST_EXCEPTION)
            for task in done:
                task.result()
        except GateInterceptException as exc:
            logger.info('In "%s": %s', self._proxy_name, exc)
        except socket.error as exc:
            logger.error('Exception in "%s": %s', self._proxy_name, exc)
        except Exception:
            logger.exception('interceptor failed')
        finally:
            for task in self._interceptor_tasks:
                task.cancel()

            # Closing the sockets here so that the self.shutdown()
            # returns only when the sockets are actually closed
            for sock in self._server, self._client:
                try:
                    sock.close()
                except socket.error:
                    logger.exception('Exception in "%s" on closing %s:', self._proxy_name, sock)


# @endcond


class BaseGate:
    """
    This base class maintain endpoints of two types:

    Server-side endpoints to receive messages from clients. Address of this
    endpoint is described by (host_for_client, port_for_client).

    Client-side endpoints to forward messages to server. Server must listen on
    (host_to_server, port_to_server).

    Asynchronously concurrently passes data from client to server and from
    server to client, allowing intercepting the data, injecting delays and
    dropping connections.

    @warning Do not use this class itself. Use one of the specifications
    TcpGate or UdpGate

    @ingroup userver_testsuite

    @see @ref scripts/docs/en/userver/chaos_testing.md
    """

    _NOT_IMPLEMENTED_MESSAGE = 'Do not use BaseGate itself, use one of specializations TcpGate or UdpGate'

    def __init__(self, route: GateRoute, loop: typing.Optional[EvLoop] = None) -> None:
        self._route = route
        if loop is None:
            loop = asyncio.get_running_loop()
        self._loop = loop

        self._to_server_intercept: Interceptor = _intercept_ok
        self._to_client_intercept: Interceptor = _intercept_ok

        self._accept_sockets: typing.List[socket.socket] = []
        self._accept_tasks: typing.List[asyncio.Task[None]] = []

        self._sockets: typing.Set[_SocketsPaired] = set()

    async def __aenter__(self) -> 'BaseGate':
        self.start()
        return self

    async def __aexit__(self, exc_type, exc_value, traceback) -> None:
        await self.stop()

    def _create_accepting_sockets(self) -> typing.List[Socket]:
        raise NotImplementedError(self._NOT_IMPLEMENTED_MESSAGE)

    def start(self):
        """Open the socket and start accepting tasks"""
        if self._accept_sockets:
            return

        self._accept_sockets.extend(self._create_accepting_sockets())

        if not self._accept_sockets:
            raise GateException(
                f'Could not resolve hostname {self._route.host_for_client}',
            )

        if self._route.port_for_client == 0:
            # In case of stop()+start() bind to the same port
            self._route = GateRoute(
                name=self._route.name,
                host_to_server=self._route.host_to_server,
                port_to_server=self._route.port_to_server,
                host_for_client=self._accept_sockets[0].getsockname()[0],
                port_for_client=self._accept_sockets[0].getsockname()[1],
            )

        self.start_accepting()

    def start_accepting(self) -> None:
        """Start accepting tasks"""
        assert self._accept_sockets
        if not all(tsk.done() for tsk in self._accept_tasks):
            return

        self._accept_tasks.clear()
        for sock in self._accept_sockets:
            self._accept_tasks.append(
                asyncio.create_task(self._do_accept(sock)),
            )

    async def stop_accepting(self) -> None:
        """
        Stop accepting tasks without closing the accepting socket.
        """
        for tsk in self._accept_tasks:
            await _cancel_and_join(tsk)
        self._accept_tasks.clear()

    async def stop(self) -> None:
        """
        Stop accepting tasks, close all the sockets
        """
        if not self._accept_sockets and not self._sockets:
            return

        await self.to_server_pass()
        await self.to_client_pass()

        await self.stop_accepting()
        logger.info('Before close() %s', self.info())
        await self.sockets_close()
        assert not self._sockets

        for sock in self._accept_sockets:
            sock.close()
        self._accept_sockets.clear()
        logger.info('Stopped. %s', self.info())

    async def sockets_close(
        self,
        *,
        count: typing.Optional[int] = None,
    ) -> None:
        """Close all the connection going through the gate"""
        for x in list(self._sockets)[0:count]:
            await x.shutdown()
        self._collect_garbage()

    def get_sockname_for_clients(self) -> Address:
        """
        Returns the client socket address that the gate listens on.

        This function allows to use 0 in GateRoute.port_for_client and retrieve
        the actual port and host.
        """
        assert self._route.port_for_client != 0, ('Gate was not started and the port_for_client is still 0',)
        return (self._route.host_for_client, self._route.port_for_client)

    def info(self) -> str:
        """Print info on open sockets"""
        if not self._sockets:
            return f'"{self._route.name}" no active sockets'

        return f'"{self._route.name}" active sockets:\n\t' + '\n\t'.join(x.info() for x in self._sockets)

    def _collect_garbage(self) -> None:
        self._sockets = {x for x in self._sockets if x.is_active()}

    async def _do_accept(self, accept_sock: Socket) -> None:
        """
        This task should wait for connections and create SocketPair
        """
        raise NotImplementedError(self._NOT_IMPLEMENTED_MESSAGE)

    async def set_to_server_interceptor(self, interceptor: Interceptor) -> callinfo.AsyncCallQueue:
        """
        Replace existing interceptor of client to server data with a custom
        """
        self._to_server_intercept = _create_callqueue(interceptor)
        for x in self._sockets:
            await x.set_to_server_interceptor(self._to_server_intercept)
        return self._to_server_intercept

    async def set_to_client_interceptor(self, interceptor: Interceptor) -> callinfo.AsyncCallQueue:
        """
        Replace existing interceptor of server to client data with a custom

        """
        if interceptor is not None:
            self._to_client_intercept = _create_callqueue(interceptor)
        else:
            self._to_client_intercept = None
        for x in self._sockets:
            await x.set_to_client_interceptor(self._to_client_intercept)
        return self._to_client_intercept

    async def to_server_pass(self) -> callinfo.AsyncCallQueue:
        """Pass data as is"""
        logging.trace('to_server_pass')
        return await self.set_to_server_interceptor(_intercept_ok)

    async def to_client_pass(self) -> callinfo.AsyncCallQueue:
        """Pass data as is"""
        logging.trace('to_client_pass')
        return await self.set_to_client_interceptor(_intercept_ok)

    async def to_server_noop(self) -> callinfo.AsyncCallQueue:
        """Do not read data, causing client to keep multiple data"""
        logging.trace('to_server_noop')
        return await self.set_to_server_interceptor(None)

    async def to_client_noop(self) -> callinfo.AsyncCallQueue:
        """Do not read data, causing server to keep multiple data"""
        logging.trace('to_client_noop')
        return await self.set_to_client_interceptor(None)

    async def to_server_drop(self) -> callinfo.AsyncCallQueue:
        """Read and discard data"""
        logging.trace('to_server_drop')
        return await self.set_to_server_interceptor(_intercept_drop)

    async def to_client_drop(self) -> callinfo.AsyncCallQueue:
        """Read and discard data"""
        logging.trace('to_client_drop')
        return await self.set_to_client_interceptor(_intercept_drop)

    async def to_server_delay(self, delay: float) -> callinfo.AsyncCallQueue:
        """Delay data transmission"""
        logging.trace('to_server_delay, delay: %s', delay)

        async def _intercept_delay_bound(
            loop: EvLoop,
            socket_from: Socket,
            socket_to: Socket,
        ) -> None:
            await _intercept_delay(delay, loop, socket_from, socket_to)

        return await self.set_to_server_interceptor(_intercept_delay_bound)

    async def to_client_delay(self, delay: float) -> callinfo.AsyncCallQueue:
        """Delay data transmission"""
        logging.trace('to_client_delay, delay: %s', delay)

        async def _intercept_delay_bound(
            loop: EvLoop,
            socket_from: Socket,
            socket_to: Socket,
        ) -> None:
            await _intercept_delay(delay, loop, socket_from, socket_to)

        return await self.set_to_client_interceptor(_intercept_delay_bound)

    async def to_server_close_on_data(self) -> callinfo.AsyncCallQueue:
        """Close on first bytes of data from client"""
        logging.trace('to_server_close_on_data')
        return await self.set_to_server_interceptor(_intercept_close_on_data)

    async def to_client_close_on_data(self) -> callinfo.AsyncCallQueue:
        """Close on first bytes of data from server"""
        logging.trace('to_client_close_on_data')
        return await self.set_to_client_interceptor(_intercept_close_on_data)

    async def to_server_corrupt_data(self) -> callinfo.AsyncCallQueue:
        """Corrupt data received from client"""
        logging.trace('to_server_corrupt_data')
        return await self.set_to_server_interceptor(_intercept_corrupt)

    async def to_client_corrupt_data(self) -> callinfo.AsyncCallQueue:
        """Corrupt data received from server"""
        logging.trace('to_client_corrupt_data')
        return await self.set_to_client_interceptor(_intercept_corrupt)

    async def to_server_limit_bps(self, bytes_per_second: float) -> callinfo.AsyncCallQueue:
        """Limit bytes per second transmission by network from client"""
        logging.trace(
            'to_server_limit_bps, bytes_per_second: %s',
            bytes_per_second,
        )
        return await self.set_to_server_interceptor(_InterceptBpsLimit(bytes_per_second))

    async def to_client_limit_bps(self, bytes_per_second: float) -> callinfo.AsyncCallQueue:
        """Limit bytes per second transmission by network from server"""
        logging.trace(
            'to_client_limit_bps, bytes_per_second: %s',
            bytes_per_second,
        )
        return await self.set_to_client_interceptor(_InterceptBpsLimit(bytes_per_second))

    async def to_server_limit_time(self, timeout: float, jitter: float) -> callinfo.AsyncCallQueue:
        """Limit connection lifetime on receive of first bytes from client"""
        logging.trace(
            'to_server_limit_time, timeout: %s, jitter: %s',
            timeout,
            jitter,
        )
        return await self.set_to_server_interceptor(_InterceptTimeLimit(timeout, jitter))

    async def to_client_limit_time(self, timeout: float, jitter: float) -> callinfo.AsyncCallQueue:
        """Limit connection lifetime on receive of first bytes from server"""
        logging.trace(
            'to_client_limit_time, timeout: %s, jitter: %s',
            timeout,
            jitter,
        )
        return await self.set_to_client_interceptor(_InterceptTimeLimit(timeout, jitter))

    async def to_server_smaller_parts(
        self,
        max_size: int,
        *,
        sleep_per_packet: float = 0,
    ) -> callinfo.AsyncCallQueue:
        """
        Pass data to server in smaller parts

        @param max_size Max packet size to send to server
        @param sleep_per_packet Optional sleep interval per packet, seconds
        """
        logging.trace('to_server_smaller_parts, max_size: %s', max_size)
        return await self.set_to_server_interceptor(
            _InterceptSmallerParts(max_size, sleep_per_packet),
        )

    async def to_client_smaller_parts(
        self,
        max_size: int,
        *,
        sleep_per_packet: float = 0,
    ) -> callinfo.AsyncCallQueue:
        """
        Pass data to client in smaller parts

        @param max_size Max packet size to send to client
        @param sleep_per_packet Optional sleep interval per packet, seconds
        """
        logging.trace('to_client_smaller_parts, max_size: %s', max_size)
        return await self.set_to_client_interceptor(
            _InterceptSmallerParts(max_size, sleep_per_packet),
        )

    async def to_server_concat_packets(self, packet_size: int) -> callinfo.AsyncCallQueue:
        """
        Pass data in bigger parts
        @param packet_size minimal size of the resulting packet
        """
        logging.trace('to_server_concat_packets, packet_size: %s', packet_size)
        return await self.set_to_server_interceptor(_InterceptConcatPackets(packet_size))

    async def to_client_concat_packets(self, packet_size: int) -> callinfo.AsyncCallQueue:
        """
        Pass data in bigger parts
        @param packet_size minimal size of the resulting packet
        """
        logging.trace('to_client_concat_packets, packet_size: %s', packet_size)
        return await self.set_to_client_interceptor(_InterceptConcatPackets(packet_size))

    async def to_server_limit_bytes(self, bytes_limit: int) -> callinfo.AsyncCallQueue:
        """Drop all connections each `bytes_limit` of data sent by network"""
        logging.trace('to_server_limit_bytes, bytes_limit: %s', bytes_limit)
        return await self.set_to_server_interceptor(_InterceptBytesLimit(bytes_limit, self))

    async def to_client_limit_bytes(self, bytes_limit: int) -> callinfo.AsyncCallQueue:
        """Drop all connections each `bytes_limit` of data sent by network"""
        logging.trace('to_client_limit_bytes, bytes_limit: %s', bytes_limit)
        return await self.set_to_client_interceptor(_InterceptBytesLimit(bytes_limit, self))

    async def to_server_substitute(self, pattern: str, repl: str) -> callinfo.AsyncCallQueue:
        """Apply regex substitution to data from client"""
        logging.trace(
            'to_server_substitute, pattern: %s, repl: %s',
            pattern,
            repl,
        )
        return await self.set_to_server_interceptor(_InterceptSubstitute(pattern, repl))

    async def to_client_substitute(self, pattern: str, repl: str) -> callinfo.AsyncCallQueue:
        """Apply regex substitution to data from server"""
        logging.trace(
            'to_client_substitute, pattern: %s, repl: %s',
            pattern,
            repl,
        )
        return await self.set_to_client_interceptor(_InterceptSubstitute(pattern, repl))


class TcpGate(BaseGate):
    """
    Implements TCP chaos-proxy logic such as accepting incoming tcp client
    connections. On each new connection new tcp client connects to server
    (host_to_server, port_to_server).

    @ingroup userver_testsuite

    @see @ref scripts/docs/en/userver/chaos_testing.md
    """

    def __init__(self, route: GateRoute, loop: typing.Optional[EvLoop] = None) -> None:
        self._connected_event = asyncio.Event()
        super().__init__(route, loop)

    def connections_count(self) -> int:
        """
        Returns maximal amount of connections going through the gate at
        the moment.

        @warning Some of the connections could be closing, or could be opened
                 right before the function starts. Use with caution!
        """
        return len(self._sockets)

    async def wait_for_connections(self, *, count=1, timeout=0.0) -> None:
        """
        Wait for at least `count` connections going through the gate.

        @throws asyncio.TimeoutError exception if failed to get the
                required amount of connections in time.
        """
        if timeout <= 0.0:
            while self.connections_count() < count:
                await self._connected_event.wait()
                self._connected_event.clear()
            return

        deadline = time.monotonic() + timeout
        while self.connections_count() < count:
            time_left = deadline - time.monotonic()
            await asyncio.wait_for(
                self._connected_event.wait(),
                timeout=time_left,
            )
            self._connected_event.clear()

    def _create_accepting_sockets(self) -> typing.List[Socket]:
        res: typing.List[Socket] = []
        for addr in socket.getaddrinfo(
            self._route.host_for_client,
            self._route.port_for_client,
            type=socket.SOCK_STREAM,
        ):
            sock = Socket(addr[0], addr[1])
            _make_socket_nonblocking(sock)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(addr[4])
            sock.listen()
            logger.debug(
                f'Accepting connections on {sock.getsockname()}, fd={sock.fileno()}',
            )
            res.append(sock)

        return res

    async def _connect_to_server(self):
        addrs = await self._loop.getaddrinfo(
            self._route.host_to_server,
            self._route.port_to_server,
            type=socket.SOCK_STREAM,
        )
        for addr in addrs:
            server = Socket(addr[0], addr[1])
            _make_socket_nonblocking(server)
            try:
                await self._loop.sock_connect(server, addr[4])
                logging.trace('Connected to %s', addr[4])
                return server
            except Exception as exc:  # pylint: disable=broad-except
                server.close()
                logging.warning('Could not connect to %s: %s', addr[4], exc)

    async def _do_accept(self, accept_sock: Socket) -> None:
        while True:
            client, _ = await self._loop.sock_accept(accept_sock)
            _make_socket_nonblocking(client)

            server = await self._connect_to_server()
            if server:
                self._sockets.add(
                    _SocketsPaired(
                        self._route.name,
                        self._loop,
                        client,
                        server,
                        self._to_server_intercept,
                        self._to_client_intercept,
                    ),
                )
                self._connected_event.set()
            else:
                client.close()

            self._collect_garbage()


class UdpGate(BaseGate):
    """
    Implements UDP chaos-proxy logic such as demuxing incoming datagrams
    from different clients.
    Separate connections to server are made for each new client.

    @ingroup userver_testsuite

    @see @ref scripts/docs/en/userver/chaos_testing.md
    """

    def __init__(self, route: GateRoute, loop: typing.Optional[EvLoop] = None):
        self._clients: typing.Set[_UdpDemuxSocketMock] = set()
        super().__init__(route, loop)

    def is_connected(self) -> bool:
        """
        Returns True if there is active pair of sockets ready to transfer data
        at the moment.
        """
        return len(self._sockets) > 0

    def _create_accepting_sockets(self) -> typing.List[Socket]:
        res: typing.List[Socket] = []
        for addr in socket.getaddrinfo(
            self._route.host_for_client,
            self._route.port_for_client,
            type=socket.SOCK_DGRAM,
        ):
            sock = socket.socket(addr[0], addr[1])
            _make_socket_nonblocking(sock)
            sock.bind(addr[4])
            logger.debug(f'Accepting connections on {sock.getsockname()}')
            res.append(sock)

        return res

    async def _connect_to_server(self):
        addrs = await self._loop.getaddrinfo(
            self._route.host_to_server,
            self._route.port_to_server,
            type=socket.SOCK_DGRAM,
        )
        for addr in addrs:
            server = Socket(addr[0], addr[1])
            try:
                _make_socket_nonblocking(server)
                await self._loop.sock_connect(server, addr[4])
                logging.trace('Connected to %s', addr[4])
                return server
            except Exception as exc:  # pylint: disable=broad-except
                logging.warning('Could not connect to %s: %s', addr[4], exc)

    def _collect_garbage(self) -> None:
        super()._collect_garbage()
        self._clients = {c for c in self._clients if c.is_active()}

    async def _do_accept(self, accept_sock: Socket):
        sock = asyncio_socket.from_socket(accept_sock)
        while True:
            data, addr = await sock.recvfrom(RECV_MAX_SIZE, timeout=60.0)

            client: typing.Optional[_UdpDemuxSocketMock] = None
            for known_clients in self._clients:
                if addr == known_clients.peer_address:
                    client = known_clients
                    break

            if client is None:
                server = await self._connect_to_server()
                if not server:
                    accept_sock.close()
                    break

                client = _UdpDemuxSocketMock(accept_sock, addr)
                self._clients.add(client)

                self._sockets.add(
                    _SocketsPaired(
                        self._route.name,
                        self._loop,
                        client,
                        server,
                        self._to_server_intercept,
                        self._to_client_intercept,
                    ),
                )

            await client.push(self._loop, data)
            self._collect_garbage()

    async def to_server_concat_packets(self, packet_size: int) -> None:
        raise NotImplementedError('Udp packets cannot be concatenated')

    async def to_client_concat_packets(self, packet_size: int) -> None:
        raise NotImplementedError('Udp packets cannot be concatenated')

    async def to_server_smaller_parts(
        self,
        max_size: int,
        *,
        sleep_per_packet: float = 0,
    ) -> None:
        raise NotImplementedError('Udp packets cannot be split')

    async def to_client_smaller_parts(
        self,
        max_size: int,
        *,
        sleep_per_packet: float = 0,
    ) -> None:
        raise NotImplementedError('Udp packets cannot be split')


def _create_callqueue(obj):
    if obj is None:
        return None

    # workaround testsuite acallqueue that does not work with instances
    if isinstance(obj, callinfo.AsyncCallQueue):
        return obj
    if hasattr(obj, '__name__'):
        return callinfo.acallqueue(obj)

    @functools.wraps(obj)
    async def wrapper(*args, **kwargs):
        return await obj(*args, **kwargs)

    return callinfo.acallqueue(wrapper)


async def _wait_for_data(sock, timeout=60.0):
    if isinstance(sock, _UdpDemuxSocketMock):
        sock = sock.get_demux_out()
    sock = asyncio_socket.from_socket(sock)
    await sock.wait_for_data(timeout=timeout)
