from argparse import ArgumentParser
import inspect
import logging
import logging.config
from pathlib import Path
import sys
import traceback

import yaml

from data_trace.command import Command
from data_trace.pack.pack_command import PackCommand

logger = logging.getLogger()


class Driver:
    def __init__(self):
        self._default_configure_logging(config_path=self._resource_path() / "logging.yaml")

        self._arg_parser = ArgumentParser(prog="dt", description="Data analysing, processing and tracing tool")
        sub_parser = self._arg_parser.add_subparsers(required=True)

        self._register_command(sub_parser=sub_parser, name=PackCommand.name(), command=PackCommand())

    def run(self) -> int:
        args = self._arg_parser.parse_args()
        exit_code = args.run_func(settings=args)

        logger.info("Finished execution")
        return exit_code


    def _register_command(self, sub_parser, name: str, command: Command):
        command_parser = sub_parser.add_parser(name=name)
        command.init_arguments(arg_parser=command_parser)
        command_parser.set_defaults(run_func=lambda settings: command.run(settings=settings))

    def _module_path(self) -> Path:
        module_name = __name__.split(".")[0]
        module = sys.modules[module_name]
        module_dir = Path(inspect.getfile(module)).parent

        return module_dir

    def _resource_path(self) -> Path:
        return self._module_path() / "resources"

    def _default_configure_logging(self, config_path: Path):
        if not config_path.exists():
            raise FileNotFoundError(f"Could not find logging configuration: {config_path}")
        if not config_path.is_file():
            raise FileNotFoundError(f"Configuration must be given as file: {config_path}")

        with open(config_path) as f:
            try:
                logging_config = yaml.safe_load(f)
            except yaml.YAMLError as ex:
                logger.error("Error while parsing configuration")
                logger.exception(ex)

        logging.config.dictConfig(logging_config)


def clean_terminate(error):
    # Terminate nicely depending on the exception

    user_errors = (
        PermissionError,
        FileExistsError,
        FileNotFoundError,
        InterruptedError,
        IsADirectoryError,
        NotADirectoryError,
        TimeoutError,
    ) + (
        # Put own exceptions here
    )

    if isinstance(error, user_errors):

        # Fetch extra error informations
        error_code = int(getattr(error, "rc", getattr(error, "errno", 1)))
        advice = getattr(error, "advice", None)
        if advice:
            logger.warning(advice)

        # Log error and exit
        logger.error(error)
        err_name = error.__class__.__name__
        logger.critical("Finished execution with error %s (%s)", err_name, error_code)
        sys.exit(error_code)

    # Developper bug catchall
    error_code = 255
    logger.error(traceback.format_exc())
    logger.critical("Uncaught error: %s", error.__class__)
    sys.exit(error_code)

def main():
    try:
        return Driver().run()
    except Exception as ex:
        clean_terminate(ex)

if __name__ == "__main__":
    main()
