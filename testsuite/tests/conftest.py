import asyncio

import pytest

pytest_plugins = ['pytest_userver.plugins.core']


@pytest.fixture(name='asyncio_loop')
async def _asyncio_loop():
    return asyncio.get_running_loop()
