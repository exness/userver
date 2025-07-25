import pytest


async def test_basic(service_client, monitor_client):
    response = await service_client.get('/metrics')
    assert response.status_code == 200
    assert 'application/json' in response.headers['Content-Type']

    metric = await monitor_client.single_metric('sample-metrics.foo')
    assert metric.value > 0


# /// [uservice_oneshot sample]
@pytest.mark.uservice_oneshot
async def test_initial_metrics(service_client, monitor_client):
    metric = await monitor_client.single_metric('sample-metrics.foo')
    assert metric.value == 0
    # /// [uservice_oneshot sample]


# /// [metrics reset]
async def test_reset(service_client, monitor_client):
    # Reset service metrics
    await service_client.reset_metrics()
    # Retrieve metrics
    metric = await monitor_client.single_metric('sample-metrics.foo')
    assert metric.value == 0
    assert not metric.labels
    # /// [metrics reset]

    response = await service_client.get('/metrics')
    assert response.status_code == 200
    assert 'application/json' in response.headers['Content-Type']

    metric = await monitor_client.single_metric('sample-metrics.foo')
    assert metric.value == 1

    await service_client.reset_metrics()
    metric = await monitor_client.single_metric('sample-metrics.foo')
    assert metric.value == 0


# /// [metrics labels]
async def test_engine_metrics(service_client, monitor_client):
    metric = await monitor_client.single_metric(
        'engine.task-processors.tasks.finished.v2',
        labels={'task_processor': 'main-task-processor'},
    )
    assert metric.value > 0
    assert metric.labels == {'task_processor': 'main-task-processor'}

    metrics_dict = await monitor_client.metrics(
        prefix='http.',
        labels={'http_path': '/ping'},
    )

    assert metrics_dict
    assert 'http.handler.cancelled-by-deadline' in metrics_dict

    assert (
        metrics_dict.value_at(
            'http.handler.in-flight',
            labels={
                'http_path': '/ping',
                'http_handler': 'handler-ping',
                'version': '2',
            },
        )
        == 0
    )
    # /// [metrics labels]
