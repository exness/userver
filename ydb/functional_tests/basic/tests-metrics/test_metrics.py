import os
import re

import pytest


@pytest.fixture(name='force_metrics_to_appear')
async def _force_metrics_to_appear(service_client):
    await service_client.update_server_state()
    await service_client.reset_metrics()

    response = await service_client.post(
        'ydb/upsert-row',
        json={
            'id': 'id-upsert',
            'name': 'name-upsert',
            'service': 'srv',
            'channel': 123,
        },
    )
    assert response.status_code == 200
    assert response.json() == {}


async def test_metrics_smoke(monitor_client, force_metrics_to_appear):
    metrics = await monitor_client.metrics()
    assert len(metrics) > 1


async def test_metrics_portability(service_client, force_metrics_to_appear):
    warnings = await service_client.metrics_portability(prefix='ydb')
    assert not warnings


def _is_ydb_metric(line: str) -> bool:
    line = line.strip()
    if line.startswith('#') or not line:
        return False

    return line.startswith('ydb.') or line.startswith('distlock.')


def _normalize_metrics(metrics: str) -> str:
    filtered = [line for line in metrics.splitlines() if _is_ydb_metric(line)]
    filtered.sort()
    without_host = (re.sub(r'YdbHost=[^,\t]*', 'YdbHost=(REDACTED)', line) for line in filtered)
    return '\n'.join(without_host)


def _hide_metrics_values(metrics: str) -> str:
    return '\n'.join(line.rsplit('\t', 1)[0] for line in metrics.splitlines())


@pytest.mark.skipif(not os.environ.get('TESTSUITE_ARCADIA_RUN'), reason='Native metrics flap in the outer world')
async def test_metrics(monitor_client, load, force_metrics_to_appear):
    ground_truth = _normalize_metrics(load('metrics_values.txt'))
    all_metrics = await monitor_client.metrics_raw(output_format='pretty')
    all_metrics = _normalize_metrics(all_metrics)
    all_metrics_paths = _hide_metrics_values(all_metrics)
    ground_truth_paths = _hide_metrics_values(ground_truth)

    assert all_metrics_paths == ground_truth_paths, (
        f'\n===== Service metrics start =====\n{all_metrics}\n===== Service metrics end =====\n'
    )
