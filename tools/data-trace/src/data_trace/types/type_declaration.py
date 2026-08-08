from abc import ABC, abstractmethod

from data_trace.pack.field import Field


class TypeDeclaration(ABC):
    def __init__(self, fields: list[Field]):
        self._fields = fields

    @staticmethod
    @abstractmethod
    def name() -> str:
        """
        Returns user-specifier for the type implementation
        """

    @staticmethod
    def exists(name: str) -> bool:
        for type_decl in TypeDeclaration.__subclasses__():
            if type_decl.name() == name:
                return True

        return False

    @property
    def fields(self) -> list[Field]:
        return self._fields

    @property
    def field_count(self) -> int:
        return len(self._fields)

    @property
    def block_size(self) -> int:
        fields = self._fields
        if not fields:
            return 0

        return sum([f.size for f in fields])
