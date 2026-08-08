from data_trace.pack.data_type import DataType
from data_trace.pack.field import Field
from data_trace.types.type_declaration import TypeDeclaration


class GpsTypeDeclaration(TypeDeclaration):
    def __init__(self):
        super().__init__(fields=[
            Field(name="x", data_type=DataType.FLOAT),
            Field(name="y", data_type=DataType.FLOAT),
            Field(name="z", data_type=DataType.FLOAT)
        ])

    @staticmethod
    def name() -> str:
        return "gps"
