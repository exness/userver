import pytest

from testsuite.databases.pgsql import discover

pytest_plugins = ['pytest_userver.plugins.postgresql']


@pytest.fixture(scope='session')
def initial_data_path(service_source_dir):
    """Path for find files with data"""
    return [service_source_dir / 'postgresql/data']


@pytest.fixture(scope='session')
def pgsql_local(service_source_dir, pgsql_local_create):
    """Create schemas databases for tests"""
    databases = discover.find_schemas(
        'auth',
        [service_source_dir.joinpath('postgresql/schemas')],
    )
    return pgsql_local_create(list(databases.values()))


@pytest.fixture(scope='session')
def service_env():
    return {
        'SERVER_DIGEST_AUTH_SECRET': ('{ "http_server_digest_auth_secret": "some-private-key" }'),
    }
