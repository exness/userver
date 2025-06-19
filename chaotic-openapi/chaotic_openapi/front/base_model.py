from typing import Any
from typing import List

import pydantic


class BaseModel(pydantic.BaseModel):
    model_config = pydantic.ConfigDict(extra='allow')
    _model_userver_tags: List[str] = []

    def model_post_init(self, __context: Any) -> None:
        if not self.__pydantic_extra__:
            return
        for field in self.__pydantic_extra__:
            if field.startswith('x-taxi-') or field.startswith('x-usrv-'):
                assert field in self._model_userver_tags, f'Field {field} is not allowed in this context'
            # ignore any other tag
