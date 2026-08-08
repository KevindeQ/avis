from abc import ABC, abstractmethod
from pathlib import Path

from data_trace.formats.property_bag import PropertyBag
from data_trace.pack.data_collector import DataCollector
from data_trace.pack.pack_definition import PackDefinition


class Format(ABC):
    @staticmethod
    @abstractmethod
    def name() -> str:
        """
        Returns user-specifier for the format implementation
        """

    @staticmethod
    def exists(name: str) -> bool:
        for format in Format.__subclasses__():
            if format.name() == name:
                return True

        return False


class FileReader:
    @abstractmethod
    def read(self, path: Path) -> PropertyBag:
        """
        Read format specific file
        """


class FileWriter:
    @abstractmethod
    def write(self, pack_definition: PackDefinition, data_collector: DataCollector):
        """
        Write format specific file
        """
