from data_trace.pack.data_type import DataType
from data_trace.pack.field import Field
from data_trace.types.type_declaration import TypeDeclaration


class OrientationTypeDeclaration(TypeDeclaration):
    def __init__(self):
        super().__init__(fields=[
            Field(name="rotation_x", data_type=DataType.FLOAT),
            Field(name="rotation_y", data_type=DataType.FLOAT),
            Field(name="rotation_z", data_type=DataType.FLOAT)
        ])

    @staticmethod
    def name() -> str:
        return "orientation"