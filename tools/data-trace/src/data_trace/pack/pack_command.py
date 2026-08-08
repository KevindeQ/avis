from argparse import ArgumentParser
from dataclasses import dataclass
import logging
from pathlib import Path
import re
from typing import Any, Type

import yaml

from data_trace.command import Command
from data_trace.exceptions import ConfigurationError, UnknownFormat
from data_trace.formats.format import FileReader, FileWriter, Format
from data_trace.pack.data_collector import DataCollector
from data_trace.pack.pack_definition import PackDefinition
from data_trace.pack.data_stream_definition import DataStreamDefinition
from data_trace.types.type_declaration import TypeDeclaration

logger = logging.getLogger()


class PackCommand(Command):
    @staticmethod
    def name() -> str:
        return "pack"

    @staticmethod
    def init_arguments(arg_parser: ArgumentParser):
        arg_parser.add_argument("config")

    def run(self, settings) -> int:
        # Check existence of configuration file
        config_path = Path(settings.config)
        if not config_path.exists():
            raise FileNotFoundError(f"Could not find pack configuration: {config_path}")
        if not config_path.is_file():
            raise FileNotFoundError(f"Configuration must be given as file: {config_path}")

        logger.info("Loading configuration...")

        # Load configuration
        with open(config_path) as f:
            try:
                pack_config = yaml.safe_load(f)
            except yaml.YAMLError as ex:
                logger.error("Error while parsing configuration")
                logger.exception(ex)

                return 1

        # Only continue if there is anything to pack
        if "pack" not in pack_config:
            logger.info(f"No pack section in the configuration")
            return 0

        # Turn all root_directory properties into absolute paths taking into account
        # parent root_directories.
        pack_config = self._resolve_root_directory_paths(pack_config)

        # Load all pack definitions from the loaded configuration
        pack_config_definitions = pack_config["pack"]["definitions"] if "definitions" in pack_config["pack"] else None
        if pack_config_definitions is None:
            logger.info("No pack definitions in the configuration")
            return 0
        if not isinstance(pack_config_definitions, list):
            logger.error("Pack definitions must be a list. Encountered another type.")
            return 1

        logger.info("Loading packing definitions...")

        pack_definitions: list[PackDefinition] = []
        for index, definition in enumerate(pack_config_definitions):
            pack_definition = self._parse_pack_definition(settings=pack_config, definition=definition, definition_index=index)
            pack_definitions.append(pack_definition)

        logger.info("Starting packing process")

        for pack_definition in pack_definitions:
            self._process_pack_definition(pack_definition)

        logger.info("Finished processing")

        return 0

    def _parse_required_definition_property(self,
                                            definition: dict,
                                            property_name: str,
                                            property_type: Type,
                                            definition_id: str | int) -> Any:
        if property_name not in definition:
            raise ConfigurationError(f"Missing property \"{property_name}\" in definition {definition_id}")

        property = definition[property_name]
        if not isinstance(property, property_type):
            raise ConfigurationError(f"Property \"{property_name}\" does not match type expectation in definition {definition_id}")

        return property

    def _parse_optional_definition_property(self,
                                            definition: dict,
                                            property_name: str,
                                            property_type: Type,
                                            definition_id: str | int) -> Any:
        property = definition[property_name] if property_name in definition else None
        if property is not None and not isinstance(property, property_type):
            raise ConfigurationError(f"Property \"{property_name}\" does not match type expectation in definition {definition_id}")

        return property

    def _parse_required_data_stream_property(self,
                                             data_stream: dict,
                                             property_name: str,
                                             property_type: Type,
                                             definition_id: str | int,
                                             data_stream_id: str | int) -> Any:
        if property_name not in data_stream:
            raise ConfigurationError(f"Missing property \"{property_name}\" in data stream {data_stream_id}")

        property = data_stream[property_name]
        if not isinstance(property, property_type):
            raise ConfigurationError(f"Property \"{property_name}\" does not match type expectation in data stream {data_stream_id} of definition {definition_id}")

        return property

    def _parse_optional_data_stream_property(self,
                                             data_stream: dict,
                                             property_name: str,
                                             property_type: Type,
                                             definition_id: str | int,
                                             data_stream_id: str | int) -> Any | None:
        property = data_stream[property_name] if property_name in data_stream else None
        if property is not None and not isinstance(property, property_type):
            raise ConfigurationError(f"Property \"{property_name}\" does not match type expectation in data stream {data_stream_id} of definition {definition_id}")

        return property

    def _parse_pack_definition(self, settings: dict, definition: dict, definition_index: int) -> PackDefinition:
        definition_name = self._parse_required_definition_property(
            definition=definition, property_name="name", property_type=str, definition_id=definition_index)

        logger.info(f"Loading definition {definition_name}...")

        output_path = Path(self._parse_required_definition_property(
            definition=definition, property_name="output", property_type=str, definition_id=definition_name))
        if not output_path.is_absolute():
            output_path = self._resolve_root_directory(
                property=f"pack.definitions[{definition_index}]",
                settings=settings
            ) / output_path

        output_format = self._parse_required_definition_property(
            definition=definition, property_name="output_format", property_type=str, definition_id=definition_name)
        if not Format.exists(output_format):
            raise UnknownFormat(f"Pack definition \"{definition_name}\" contains reference to unknown output_format \"{output_format}\"")

        output_format_arguments = self._parse_optional_definition_property(
            definition=definition, property_name="output_format_arguments", property_type=dict, definition_id=definition_name)

        pack_definition = PackDefinition(
            name=definition_name,
            output_path=output_path,
            output_format=output_format,
            output_format_arguments=output_format_arguments)

        data_stream_definitions = self._parse_required_definition_property(
            definition=definition, property_name="data_streams", property_type=list, definition_id=pack_definition.name)
        for data_stream_index, data_stream in enumerate(data_stream_definitions):
            data_stream_name = self._parse_required_data_stream_property(
                data_stream=data_stream,
                property_name="name",
                property_type=str,
                definition_id=pack_definition.name,
                data_stream_id=data_stream_index)

            logger.info(f"Loading data stream {data_stream_name}...")

            data_stream_definition = DataStreamDefinition(name=data_stream_name)

            file_search_patterns = self._parse_required_data_stream_property(
                data_stream=data_stream,
                property_name="files",
                property_type=list,
                definition_id=pack_definition.name,
                data_stream_id=data_stream_definition.name)
            data_stream_definition.add_files_from_search_patterns(
                base_path=self._resolve_root_directory(
                    property=f"pack.definitions[{definition_index}].data_streams[{data_stream_index}]",
                    settings=settings),
                search_patterns=file_search_patterns
            )

            data_stream_definition.input_format = self._parse_required_data_stream_property(
                data_stream=data_stream,
                property_name="input_format",
                property_type=str,
                definition_id=pack_definition.name,
                data_stream_id=data_stream_definition.name)
            if not Format.exists(data_stream_definition.input_format):
                raise UnknownFormat(f"Data stream definition \"{data_stream_definition.name}\" contains reference to unknown input_format \"{data_stream_definition.input_format}\"")

            data_stream_definition.type_declaration = self._parse_required_data_stream_property(
                data_stream=data_stream,
                property_name="type_declaration",
                property_type=str,
                definition_id=pack_definition.name,
                data_stream_id=data_stream_definition.name)
            if not TypeDeclaration.exists(data_stream_definition.type_declaration):
                raise UnknownFormat(f"Data stream definition \"{data_stream_definition.name}\" contains reference to unknown type_declaration \"{data_stream_definition.type_declaration}\"")

            data_stream_definition.field_name_mappings = self._parse_optional_data_stream_property(
                data_stream=data_stream,
                property_name="mapping",
                property_type=dict,
                definition_id=pack_definition.name,
                data_stream_id=data_stream_definition.name)

            pack_definition.add_data_stream(data_stream=data_stream_definition)

        return pack_definition

    def _resolve_root_directory_paths(self, settings: dict) -> dict:
        def _resolve(settings: Any, base_path: Path) -> dict:
            if isinstance(settings, dict):
                # Update root_directory if necessary
                if "root_directory" in settings:
                    root_directory = Path(settings["root_directory"])

                    if root_directory.is_absolute():
                        base_path = root_directory
                    else:
                        base_path = base_path / root_directory
                        settings["root_directory"] = str(base_path)

                resolved_settings: dict = {}
                for key, value in settings.items():
                    resolved_settings[key] = _resolve(value, base_path=base_path)

                return resolved_settings
            elif isinstance(settings, list):
                return [_resolve(settings=item, base_path=base_path) for item in settings]

            return settings

        return _resolve(settings, Path("."))

    def _resolve_root_directory(self, property: str, settings: dict) -> Path:
        def _resolve(settings: Any, keys: list[str], base_path: Path) -> Path:
            if isinstance(settings, dict) and "root_directory" in settings:
                root_directory = Path(settings["root_directory"])

                if root_directory.is_absolute():
                    base_path = root_directory
                else:
                    base_path = base_path / root_directory

            # Return current root_directory if this is the last element in the property path
            if len(keys) == 0:
                return base_path

            # Special handling of list items
            key = keys[0]
            list_index: int | None = None
            if key not in settings:
                # Check if it is a key for a list
                match = re.match(r"^([A-Za-z\d_\.]+)\[(\d)+\]$", key)
                if match is None:
                    raise KeyError(f"Could not find {key} in {settings}")

                key = match.group(1)
                list_index = int(match.group(2))

            # Recursively traverse property path
            if isinstance(settings[key], dict):
                return _resolve(settings=settings[key], keys=keys[1:], base_path=base_path)
            elif isinstance(settings[key], list):
                if list_index is None:
                    raise RuntimeError(f"Could not index list item for property \"{key}\"")
                return _resolve(settings=settings[key][list_index], keys=keys[1:], base_path=base_path)
            else:
                raise RuntimeError(f"Property \"{'.'.join(keys)}\" could not be resolved")

        if not property:
            raise ValueError("Property must not be empty")

        keys = property.split(sep=".")
        if len(keys) == 0:
            raise RuntimeError(f"Cannot resolve path for empty property")

        return _resolve(settings=settings, keys=keys, base_path=Path("."))

    def _process_pack_definition(self, pack_definition: PackDefinition):
        logger.info(f"Reading data for data pack \"{pack_definition.name}\"...")

        encountered_error = False

        data_collector = DataCollector()
        for data_stream in pack_definition.data_streams:
            try:
                input_reader = self._build_reader(data_stream.input_format)

                for file in data_stream.files:
                    try:
                        property_bag = input_reader.read(file)
                        data_collector.add_property_bag(collection_id=data_stream.name, property_bag=property_bag)
                    except RuntimeError as ex:
                        logger.error(f"Unable to read file \"{file.name}\". Error message:")
                        logger.exception(ex)

                        encountered_error = True
            except UnknownFormat as ex:
                encountered_error = True

        # Stop execution when an error occurred during reading
        if encountered_error:
            logger.error(f"An error was encountered during data loading. Stopping further processing of data pack \"{pack_definition.name}\".")
            return

        logger.info(f"Creating data pack \"{pack_definition.name}\"...")

        # Write output
        output_writer = self._build_writer(
            pack_definition.output_format,
            format_arguments=pack_definition.output_format_arguments)
        output_writer.write(pack_definition=pack_definition, data_collector=data_collector)

        logger.info(f"Finished writing data pack \"{pack_definition.name}\"")

    def _build_reader(self, input_format: str) -> FileReader:
        for format in Format.__subclasses__():
            if format not in FileReader.__subclasses__():
                continue

            if format.name() == input_format:
                return format()

        raise UnknownFormat(f"Configuration contains reference to unknown reader format \"{input_format}\"")

    def _build_writer(self, output_format: str, format_arguments: dict) -> FileWriter:
        for format in Format.__subclasses__():
            if format not in FileWriter.__subclasses__():
                continue

            if format.name() == output_format:
                return format(**format_arguments)

        raise UnknownFormat(f"Configuration contains reference to unknown writer format \"{output_format}\"")
