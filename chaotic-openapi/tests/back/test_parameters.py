from chaotic_openapi.back.cpp_client import types
import pytest

from chaotic.back.cpp import type_name
from chaotic.back.cpp import types as cpp_types
from chaotic.front import types as chaotic_types


def test_parameters(translate_single_schema):
    schema = {
        'openapi': '3.0.0',
        'info': {'title': '', 'version': '1.0'},
        'paths': {
            '/': {
                'get': {
                    'parameters': [
                        {
                            'in': 'query',
                            'name': 'param',
                            'description': 'parameter description',
                            'schema': {
                                'type': 'integer',
                            },
                        },
                    ],
                    'responses': {},
                },
            },
        },
    }
    assert translate_single_schema(schema) == types.ClientSpec(
        client_name='test',
        cpp_namespace='test_namespace',
        operations=[
            types.Operation(
                method='GET',
                path='/',
                parameters=[
                    types.Parameter(
                        description='parameter description',
                        raw_name='param',
                        cpp_name='param',
                        cpp_type=cpp_types.CppPrimitiveType(
                            raw_cpp_type=type_name.TypeName('int'),
                            user_cpp_type=None,
                            nullable=True,
                            json_schema=chaotic_types.Integer(
                                type='integer',
                            ),
                            validators=cpp_types.CppPrimitiveValidator(prefix='/paths/[/]/get/parameters/0/schema'),
                        ),
                        parser='openapi::TrivialParameter<openapi::In::kQuery, kparam, int, int>',
                        required=False,
                    )
                ],
                request_bodies=[],
                responses=[],
            )
        ],
    )


def test_parameters_ref(translate_single_schema):
    schema = {
        'openapi': '3.0.0',
        'info': {'title': '', 'version': '1.0'},
        'paths': {
            '/': {
                'get': {
                    'parameters': [
                        {
                            '$ref': '#/components/parameters/Parameter',
                        },
                    ],
                    'responses': {},
                },
            },
        },
        'components': {
            'parameters': {
                'Parameter': {
                    'in': 'query',
                    'name': 'param',
                    'description': 'parameter description',
                    'schema': {
                        'type': 'integer',
                    },
                },
            }
        },
    }
    assert translate_single_schema(schema) == types.ClientSpec(
        client_name='test',
        cpp_namespace='test_namespace',
        operations=[
            types.Operation(
                method='GET',
                path='/',
                parameters=[
                    types.Parameter(
                        description='parameter description',
                        raw_name='param',
                        cpp_name='param',
                        cpp_type=cpp_types.CppPrimitiveType(
                            raw_cpp_type=type_name.TypeName('int'),
                            user_cpp_type=None,
                            nullable=True,
                            json_schema=chaotic_types.Integer(
                                type='integer',
                            ),
                            validators=cpp_types.CppPrimitiveValidator(
                                prefix='/components/parameters/Parameter/schema'
                            ),
                        ),
                        parser='openapi::TrivialParameter<openapi::In::kQuery, kparam, int, int>',
                        required=False,
                    )
                ],
                request_bodies=[],
                responses=[],
            )
        ],
    )


def test_parameters_schemas_ref(translate_single_schema):
    schema = {
        'openapi': '3.0.0',
        'info': {'title': '', 'version': '1.0'},
        'paths': {
            '/': {
                'get': {
                    'parameters': [
                        {
                            'in': 'query',
                            'name': 'param',
                            'description': 'parameter description',
                            'schema': {
                                '$ref': '#/components/schemas/Parameter',
                            },
                        },
                    ],
                    'responses': {},
                },
            },
        },
        'components': {
            'schemas': {
                'Parameter': {
                    'type': 'integer',
                },
            }
        },
    }
    assert translate_single_schema(schema) == types.ClientSpec(
        client_name='test',
        cpp_namespace='test_namespace',
        operations=[
            types.Operation(
                method='GET',
                path='/',
                parameters=[
                    types.Parameter(
                        description='parameter description',
                        raw_name='param',
                        cpp_name='param',
                        cpp_type=cpp_types.CppRef(
                            raw_cpp_type=type_name.TypeName(''),
                            user_cpp_type=None,
                            nullable=True,
                            json_schema=chaotic_types.Ref(
                                ref='<inline>#/components/schemas/Parameter',
                                indirect=False,
                                self_ref=False,
                                schema=chaotic_types.Integer(
                                    type='integer',
                                ),
                            ),
                            indirect=False,
                            self_ref=False,
                            orig_cpp_type=cpp_types.CppPrimitiveType(
                                raw_cpp_type=type_name.TypeName('int'),
                                user_cpp_type=None,
                                nullable=False,
                                json_schema=chaotic_types.Integer(
                                    type='integer',
                                ),
                                validators=cpp_types.CppPrimitiveValidator(
                                    prefix='Parameter',
                                    namespace='::test_namespace',
                                ),
                            ),
                        ),
                        parser='openapi::TrivialParameter<openapi::In::kQuery, kparam, int, int>',
                        required=False,
                    )
                ],
                request_bodies=[],
                responses=[],
            )
        ],
        schemas={
            '::test_namespace::Parameter': cpp_types.CppPrimitiveType(
                raw_cpp_type=type_name.TypeName('int'),
                json_schema=chaotic_types.Integer(
                    type='integer',
                ),
                nullable=False,
                user_cpp_type=None,
                default=None,
                validators=cpp_types.CppPrimitiveValidator(
                    prefix='Parameter',
                    namespace='::test_namespace',
                ),
            ),
        },
    )


def test_parameters_too_complex_schema(translate_single_schema):
    schema = {
        'openapi': '3.0.0',
        'info': {'title': '', 'version': '1.0'},
        'paths': {
            '/': {
                'get': {
                    'parameters': [
                        {
                            'in': 'query',
                            'name': 'param',
                            'description': 'parameter description',
                            'schema': {
                                'type': 'object',
                                'properties': {},
                                'additionalProperties': False,
                            },
                        },
                    ],
                    'responses': {},
                },
            },
        },
    }
    with pytest.raises(Exception) as exc:
        translate_single_schema(schema)
    assert (
        str(exc.value)
        == """
===============================================================
Unhandled error while processing <inline>
Path "/paths/[/]/get/parameters/0/schema", Format "jsonschema"
Error:
Unsupported parameter type
==============================================================="""
    )
