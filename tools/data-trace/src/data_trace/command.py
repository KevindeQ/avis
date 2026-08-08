from abc import ABC, abstractmethod
from argparse import ArgumentParser


class Command(ABC):
    @staticmethod
    @abstractmethod
    def name() -> str:
        """
        User-facing name of the command
        """

    @staticmethod
    @abstractmethod
    def init_arguments(arg_parser: ArgumentParser):
        """
        Register any command specific argument parsing rules
        """

    @abstractmethod
    def run(self, settings) -> int:
        """
        Executes the command
        """