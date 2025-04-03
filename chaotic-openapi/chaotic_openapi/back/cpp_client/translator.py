from chaotic import cpp_names
from chaotic.back.cpp import translator as chaotic_translator
from chaotic.front import ref_resolver
from chaotic.front import types as chaotic_types
from chaotic_openapi.back.cpp_client import types
from chaotic_openapi.front import model


class Translator:
    def __init__(self, service: model.Service, cpp_namespace: str) -> None:
        self._spec = types.ClientSpec(
            client_name=service.name,
            description=service.description,
            cpp_namespace=cpp_namespace,
            operations=[],
        )

        for operation in service.operations:
            op = types.Operation(
                method=operation.method.upper(),
                path=operation.path,
                description=operation.description,
                parameters=[self._translate_parameter(parameter) for parameter in operation.parameters],
                request_bodies=[
                    self._translate_request_body(content_type, body)
                    for content_type, body in operation.requestBody.items()
                ],
                responses=[],  # TODO
            )
            self._spec.operations.append(op)

        # TODO: schemas
        # TODO: responses
        # TODO: parameters

    def _translate_request_body(self, content_type: str, request_body: model.RequestBody) -> types.RequestBody:
        # TODO: $ref
        parsed_schemas = chaotic_types.ParsedSchemas(
            schemas={str(request_body.source_location()): request_body},
        )
        # TODO: components/schemas (external schemas)
        resolved_schemas = ref_resolver.RefResolver().sort_schemas(parsed_schemas)
        gen = chaotic_translator.Generator(
            chaotic_translator.GeneratorConfig(namespaces={request_body.source_location().filepath: ''})
        )
        gen_types = gen.generate_types(resolved_schemas)

        assert len(gen_types) == 1
        schema = list(gen_types.values())[0]

        return types.RequestBody(
            content_type=content_type,
            schema=schema,
        )

    def _translate_schema_type(self, schema: chaotic_types.Schema) -> str:
        if isinstance(schema, chaotic_types.Boolean):
            return 'bool'
        if isinstance(schema, chaotic_types.Integer):
            if schema.format == chaotic_types.IntegerFormat.INT32:
                return 'std::int32_t'
            if schema.format == chaotic_types.IntegerFormat.INT64:
                return 'std::int64_t'
            return 'int'
        if isinstance(schema, chaotic_types.Number):
            return 'double'
        if isinstance(schema, chaotic_types.String):
            return 'std::string'
        if isinstance(schema, chaotic_types.Array):
            item_type = self._translate_schema_type(schema.items)
            return f'std::vector<{item_type}>'
        assert False, 'Not implemented'

    def _translate_parameter(self, parameter: model.Parameter) -> types.Parameter:
        in_ = parameter.in_
        in_str = in_[0].title() + in_[1:]

        if isinstance(parameter.schema, chaotic_types.Array):
            delimiter = {
                model.Style.matrix: ',',
                model.Style.label: '.',
                model.Style.form: ',',
                model.Style.simple: ',',
                model.Style.spaceDelimited: ' ',
                model.Style.pipeDelimited: '|',
            }[parameter.style]
            raw_item_type = self._translate_schema_type(parameter.schema.items)
            user_item_type = raw_item_type
            cpp_type = self._translate_schema_type(parameter.schema)
            parser = f"openapi::ArrayParameter<openapi::In::k{in_str}, k{parameter.name}, '{delimiter}', {raw_item_type}, {user_item_type}>"
        else:
            raw_type = self._translate_schema_type(parameter.schema)
            user_type = raw_type
            cpp_type = user_type
            parser = f'openapi::TrivialParameter<openapi::In::k{in_str}, k{parameter.name}, {raw_type}, {user_type}>'

        if not parameter.required:
            cpp_type = f'std::optional<{cpp_type}>'

        return types.Parameter(
            description=parameter.description,
            raw_name=parameter.name,
            cpp_name=cpp_names.cpp_identifier(parameter.name),
            cpp_type=cpp_type,
            parser=parser,
            required=parameter.required,
        )

    def spec(self) -> types.ClientSpec:
        return self._spec
