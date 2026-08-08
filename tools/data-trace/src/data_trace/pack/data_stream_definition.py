import logging
from pathlib import Path

from data_trace.exceptions import ConfigurationError
from data_trace.pack.field import Field

logger = logging.getLogger()


class DataStreamDefinition:
    def __init__(self, name: str):
        self._name = name
        self._files: list[Path] = []

        self._input_format = ""
        self._input_type = ""
        self._mapping: dict[str, str] = {}

        self._fields: list[Field] = []

    @property
    def name(self) -> str:
        return self._name

    @property
    def files(self) -> list[Path]:
        return self._files

    @property
    def input_format(self) -> str:
        return self._input_format

    @input_format.setter
    def input_format(self, value: str):
        self._input_format = value

    @property
    def type_declaration(self) -> str:
        return self._input_type

    @type_declaration.setter
    def type_declaration(self, value: str):
        self._input_type = value

    @property
    def field_name_mappings(self) -> dict[str, str]:
        return self._mapping

    @field_name_mappings.setter
    def field_name_mappings(self, value: dict[str, str]):
        self._mapping = value

    def add_files_from_search_pattern(self, base_path: Path, search_pattern: str):
        if not isinstance(search_pattern, str):
            raise ConfigurationError(f"Search patterns must be a string")

        search_pattern_path = Path(search_pattern)
        if search_pattern_path.is_absolute():
            raise ConfigurationError(f"Search patterns must be given as a relative path")

        for file in base_path.glob(search_pattern):
            if not file.is_file():
                logger.warning(f"Search pattern \"{search_pattern}\" resulted in a path that is not a file")
                continue

            self._files.append(file)

    def add_files_from_search_patterns(self, base_path: Path, search_patterns: list[str]):
        if not isinstance(search_patterns, list):
            raise ConfigurationError(f"Search patterns must be a list of strings")

        for search_pattern in search_patterns:
            self.add_files_from_search_pattern(base_path=base_path, search_pattern=search_pattern)

    def remap_name(self, name: str) -> str:
        return self._mapping.get(name, name)
