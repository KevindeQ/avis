from pathlib import Path

from data_trace.pack.data_stream_definition import DataStreamDefinition


class PackDefinition:
    def __init__(self,
                 name: str,
                 output_path: Path,
                 output_format: str,
                 output_format_arguments: dict = {}):
        self._name = name
        self._output_path = output_path
        self._output_format = output_format
        self._output_format_arguments = output_format_arguments
        self._data_streams: list[DataStreamDefinition] = []

    @property
    def name(self) -> str:
        return self._name

    @property
    def output_path(self) -> Path:
        return self._output_path

    @property
    def output_format(self) -> str:
        return self._output_format

    @property
    def output_format_arguments(self) -> dict:
        return self._output_format_arguments

    @property
    def data_streams(self) -> list[DataStreamDefinition]:
        return self._data_streams

    @property
    def data_stream_count(self) -> int:
        return len(self._data_streams)

    def add_data_stream(self, data_stream: DataStreamDefinition):
        self._data_streams.append(data_stream)