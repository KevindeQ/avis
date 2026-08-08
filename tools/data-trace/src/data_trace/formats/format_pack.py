import logging
from os import SEEK_SET
import struct
from typing import Any, Generator, TypeVar

from data_trace.exceptions import UnknownFormat
from data_trace.formats.format import FileWriter, Format
from data_trace.pack.data_collector import DataCollector
from data_trace.pack.data_type import DataType
from data_trace.pack.field import Field
from data_trace.pack.pack_definition import PackDefinition
from data_trace.types.type_declaration import TypeDeclaration

logger = logging.getLogger()
Element = TypeVar("Element")


class PackFormat(Format, FileWriter):
    def __init__(self, page_size: int = 8192):
        self._page_size = page_size
        self._page_count = 0

    @staticmethod
    def name() -> str:
        return "pack"

    def write(self, pack_definition: PackDefinition, data_collector: DataCollector):
        # Ensure the output folder exists
        output_path = pack_definition.output_path
        output_path.parent.mkdir(parents=True, exist_ok=True)

        logger.info(f"Writing definition \"{pack_definition.name}\" to file \"{output_path.name}\" in directory \"{output_path.parent}\"")

        with open(file=output_path, mode="wb+") as file:
            # Write file header
            file.write(self._signature())
            file.write(self._version())

            # Write page configuration
            self._page_size.to_bytes(length=4, byteorder="little", signed=False)
            self._page_count.to_bytes(length=4, byteorder="little", signed=False)

            # Write stream descriptors
            file.write(pack_definition.data_stream_count.to_bytes(length=1, byteorder="little", signed=False))

            # Keep track of the file offsets for the data stream's initial page number. This is to fix the
            # page number after the actual initial page is known.
            page_number_offsets: list[int] = []
            for data_stream in pack_definition.data_streams:
                page_number_offsets.append(file.tell())

                # Initially set all page numbers to 0 because the actual page numbers are not
                # known before writing all data.
                page_number = 0
                file.write(page_number.to_bytes(length=4, byteorder="little", signed=False))

                type_decl = self._build_type_declaration(type_decl=data_stream.type_declaration)
                file.write(type_decl.field_count.to_bytes(length=1, byteorder="little", signed=False))

                offset = 0
                for field in type_decl.fields:
                    # Field offset in number of bytes from the begiing of the data block
                    file.write(offset.to_bytes(length=1, byteorder="little", signed=False))

                    # Size of the field in number of bytes
                    field_size = field.size
                    file.write(field_size.to_bytes(length=1, byteorder="little", signed=False))

                    field_type = field.data_type
                    file.write(field_type.value.to_bytes(length=1, byteorder="little", signed=False))

                    offset += field_size

            # Keep track of the realized page numbers. This is to fix the page number after the actual
            # initial page is known.
            page_numbers: list[int] = []

            # Write stream data
            current_page = 0
            for data_stream in pack_definition.data_streams:
                page_numbers.append(current_page)

                type_decl = self._build_type_declaration(type_decl=data_stream.type_declaration)

                block_size = type_decl.block_size
                blocks_per_page, block_padding = divmod(self._page_size, block_size)

                data_blocks = data_collector.find_by_id(collection_id=data_stream.name)
                for chunk in self._chunks(lst=data_blocks, chunk_size=blocks_per_page):
                    file.write(self._page_id(page_number=current_page - 1))
                    file.write(self._page_id(page_number=current_page + 1))

                    for data_block in chunk:
                        for field in type_decl.fields:
                            value = getattr(data_block, data_stream.remap_name(field.name))
                            file.write(self._to_bytes(field=field, value=value))

                    # Fill any remaining size on a page with filler bytes
                    padding = bytes([0x00]) * block_padding
                    file.write(padding)

                    current_page += 1

            # Fill in the total number of pages
            page_count_offset = 9
            file.seek(page_count_offset, SEEK_SET)
            file.write(current_page.to_bytes(length=4, byteorder="little", signed=False))

            # Fill in the initial page numbers for each data stream
            if len(page_number_offsets) != len(page_numbers):
                raise RuntimeError("Difference in declared data streams and realized data streams")
            for page_offset, page_number in zip(page_number_offsets, page_numbers):
                file.seek(page_offset, SEEK_SET)
                file.write(page_number.to_bytes(length=4, byteorder="little", signed=False))

    def _signature(self) -> bytes:
        return "AVIS".encode()

    def _version(self) -> bytes:
        version = 1
        return version.to_bytes(length=1, byteorder="little", signed=False)

    def _page_id(self, page_number: int) -> bytes:
        page_id = page_number
        if page_number < 0:
            page_id = 0xFFFFFFFF

        return page_id.to_bytes(length=4, byteorder="little", signed=False)

    def _chunks(self, lst: list[Element], chunk_size: int) -> Generator[list[Element], None, None]:
        for index in range(0, len(lst), chunk_size):
            yield lst[index:index + chunk_size]

    def _to_bytes(self, field: Field, value: Any) -> bytes:
        def int_value_mapper(f: Field, v: Any) -> bytes:
            return int(v).to_bytes(length=f.size, byteorder="little", signed=f.signed)

        def float_value_mapper(f: Field, v: Any) -> bytes:
            return struct.pack("<f", float(v))

        def double_value_mapper(f: Field, v: Any) -> bytes:
            return struct.pack("<d", float(v))

        value_map_func = {
            DataType.BOOL: lambda f, v: bool(v).to_bytes(length=f.size, byteorder="little", signed=f.signed),

            DataType.INT_8: int_value_mapper,
            DataType.INT_16: int_value_mapper,
            DataType.INT_32: int_value_mapper,
            DataType.INT_64: int_value_mapper,

            DataType.UNSIGNED_INT_8: int_value_mapper,
            DataType.UNSIGNED_INT_16: int_value_mapper,
            DataType.UNSIGNED_INT_32: int_value_mapper,
            DataType.UNSIGNED_INT_64: int_value_mapper,

            DataType.FLOAT: float_value_mapper,
            DataType.DOUBLE: double_value_mapper
        }.get(field.data_type)

        if value_map_func is None:
            raise ValueError(f"Unknown data type \"{field.data_type.name}\"")

        return value_map_func(field, value)

    def _build_type_declaration(self, type_decl: str) -> TypeDeclaration:
        for type_declaration in TypeDeclaration.__subclasses__():
            if type_declaration.name() == type_decl:
                return type_declaration()

        raise UnknownFormat(f"Configuration contains reference to unknown type declaration \"{type_decl}\"")
