# /// [Functional test]
async def test_hello_base(service_client, mockserver):
    @mockserver.handler('test/test')
    def test(request):
        assert request.query['name'] == 'john'
        return mockserver.make_response('"tost"')

    response = await service_client.get('/hello', params={'name': 'john'})
    assert response.status == 200
    assert response.text == 'tost'
    # /// [Functional test]
