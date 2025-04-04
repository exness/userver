import dataclasses
from typing import Dict
from typing import List
from typing import Optional
from typing import Set

from chaotic import cpp_names
from chaotic.back.cpp import types as cpp_types


@dataclasses.dataclass
class Parameter:
    description: str
    raw_name: str
    cpp_name: str
    cpp_type: cpp_types.CppType
    parser: str
    required: bool

    def declaration_includes(self) -> List[str]:
        # TODO
        if not self.required:
            return ['optional']
        return []


@dataclasses.dataclass
class Body:
    content_type: str
    schema: Optional[cpp_types.CppType]

    def cpp_body_type(self) -> str:
        name = cpp_names.camel_case(
            cpp_names.cpp_identifier(self.content_type),
        )
        return f'Body{name}'


@dataclasses.dataclass
class Response:
    status: int
    body: Dict[str, cpp_types.CppType]
    headers: List[Parameter] = dataclasses.field(default_factory=list)

    def is_error(self) -> bool:
        return self.status >= 400


@dataclasses.dataclass
class Operation:
    method: str
    path: str

    description: str = ''
    parameters: List[Parameter] = dataclasses.field(default_factory=list)
    request_bodies: List[Body] = dataclasses.field(default_factory=list)
    responses: List[Response] = dataclasses.field(default_factory=list)

    client_generate: bool = True

    def cpp_namespace(self) -> str:
        return cpp_names.namespace(self.path + '_' + self.method)

    def cpp_method_name(self) -> str:
        return cpp_names.camel_case(
            cpp_names.namespace(self.path + '_' + self.method),
        )

    def empty_request(self) -> bool:
        if self.parameters:
            return False
        for body in self.request_bodies:
            if body.schema:
                return False
        return True


@dataclasses.dataclass
class ClientSpec:
    client_name: str
    cpp_namespace: str
    description: str = ''
    operations: List[Operation] = dataclasses.field(default_factory=list)

    schemas: Dict[str, cpp_types.CppType] = dataclasses.field(default_factory=dict)

    def has_multiple_content_type_request(self) -> bool:
        for op in self.operations:
            if len(op.request_bodies) > 1:
                return True
        return False

    def requests_declaration_includes(self) -> List[str]:
        includes: Set[str] = set()
        for op in self.operations:
            for body in op.request_bodies:
                if body.schema:
                    includes.update(body.schema.declaration_includes())
            for param in op.parameters:
                includes.update(param.declaration_includes())

        return list(includes)
