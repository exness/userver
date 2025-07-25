import asyncio
from contextlib import asynccontextmanager
from dataclasses import dataclass
import datetime
import logging
import random
import socket
import string
import struct

import pytest
from pytest_userver import chaos

logger = logging.getLogger(__name__)
sys_random = random.SystemRandom()

RESPONSE_FLAG = struct.unpack('!H', b'\x80\x00')[0]
REFUSED_RESPONSE_FLAG = struct.unpack('!H', b'\x80\x05')[0]


def _set_response(value: bytes, refuse: bool) -> bytes:
    res = struct.unpack('!H', value)[0]
    if refuse:
        res |= REFUSED_RESPONSE_FLAG
    else:
        res |= RESPONSE_FLAG

    return struct.pack('!H', res)


@dataclass
class DnsInfo:
    host: str = '::1'
    port: int = 1053


class DnsServerProtocol:
    def __init__(self, name: str):
        self.name = name
        self.times_called = 0
        self.queries: list = []
        self.refuse = False

    def set_refuse_responses(self) -> None:
        self.refuse = True

    def set_ok_responses(self) -> None:
        self.refuse = False

    def reset_stats(self) -> None:
        self.times_called = 0

    def get_stats(self) -> int:
        return self.times_called

    def get_queries(self) -> list:
        return self.queries

    def connection_made(self, transport):
        self.transport = transport  # pylint: disable=attribute-defined-outside-init

    def connection_lost(self, exc):
        logger.info('Dns server "%s" lost connection', self.name)

    def datagram_received(self, data, addr):
        logger.info(
            f'Dns server "{self.name}" received {len(data)} bytes from {addr} at {datetime.datetime.now()}',
        )
        self.times_called += 1

        assert len(data) == 32

        # Copy request data
        response: bytes = b''
        response += data[0:2]  # 2 bytes txn id
        response += _set_response(data[2:4], self.refuse)  # 2 bytes flags
        response += data[4:6]  # 2 bytes queries count
        response += b'\x00\x01'  # 2 bytes answers count
        response += data[8:]  # Copy queries
        logger.info(f'requested query {data[8:]}')

        self.queries.append(f'{data[13:23].decode()}.{data[24:27].decode()}')

        query_type = data[-4:-2]

        response += b'\xc0\x0c'  # compression pointer to qname
        response += query_type  # query type
        response += struct.pack('!H', 1)  # class IN
        response += struct.pack('!I', 99999)  # TTL

        if query_type == b'\x00\x01':  # type A record
            response += struct.pack('!H', 4)  # data length (IPv4)
            response += socket.inet_pton(
                socket.AF_INET,
                '77.88.55.55',
            )  # our fake IPv4 address
        elif query_type == b'\x00\x1c':  # type AAAA record
            response += struct.pack('!H', 16)  # data length (IPv6)
            response += socket.inet_pton(
                socket.AF_INET6,
                '2a02:6b8:a::a',
            )  # our fake IPv6 address
        else:
            raise Exception('unknown type')

        logger.info(
            f'Dns "{self.name}" sends {len(response)} bytes to {addr} '
            f'at {datetime.datetime.now()} (refuse = {self.refuse})',
        )
        self.transport.sendto(response, addr)


@asynccontextmanager
async def create_server(dns_info, name):
    loop = asyncio.get_running_loop()
    transport, protocol = await loop.create_datagram_endpoint(
        lambda: DnsServerProtocol(name),  # pylint: disable=unnecessary-lambda
        family=socket.AF_INET6,
        local_addr=(dns_info.host, dns_info.port),
        reuse_port=True,
    )
    try:
        yield protocol
    finally:
        transport.close()


@pytest.fixture(scope='session', name='dns_mock')
async def _dns_mock(dns_info):
    async with create_server(dns_info, 'primary') as server:
        yield server


@pytest.fixture(scope='function', name='dns_mock2_lazy')
async def _dns_mock2_lazy(dns_info2):
    return create_server(dns_info2, 'secondary')


@pytest.fixture(scope='session', name='dns_info')
def _dns_info(choose_free_port) -> DnsInfo:
    return DnsInfo('::1', choose_free_port(11053))


@pytest.fixture(scope='session', name='dns_info2')
def _dns_info2(for_dns_gate_port2) -> DnsInfo:
    return DnsInfo('::1', for_dns_gate_port2)


@pytest.fixture(scope='function', name='dns_mock_stats')
def _dns_mock_stats(dns_mock):
    dns_mock.reset_stats()

    class _DnsStats:
        def get_stats(self) -> int:
            return dns_mock.get_stats()

        def get_queries(self) -> list:
            return dns_mock.get_queries()

    return _DnsStats()


@pytest.fixture
async def _gate_started(for_dns_gate_port, dns_info, dns_mock):
    gate_config = chaos.GateRoute(
        name='udp proxy',
        host_for_client='::1',
        port_for_client=for_dns_gate_port,
        host_to_server='::1',
        port_to_server=dns_info.port,
    )
    async with chaos.UdpGate(gate_config) as proxy:
        yield proxy


@pytest.fixture
def extra_client_deps(_gate_started):
    pass


@pytest.fixture(name='gate')
async def _gate_ready(service_client, _gate_started):
    await _gate_started.to_server_pass()
    await _gate_started.to_client_pass()
    await _gate_started.sockets_close()  # close keepalive connections

    yield _gate_started


@pytest.fixture(scope='function')
def gen_domain_name():
    def _gen_domain_name(length: int = 10, tld: str = '.com'):
        domain = ''.join(sys_random.choice(string.ascii_lowercase) for _ in range(length))
        return domain + tld

    return _gen_domain_name
