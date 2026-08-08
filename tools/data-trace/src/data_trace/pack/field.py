from data_trace.pack.data_type import DataType


class Field:
    def __init__(self, name: str, data_type: DataType, byte_size: int = 0):
        self._name = name
        self._data_type = data_type

        if byte_size < 0:
            raise ValueError(f"Size must be 0 (automatic size deduction) or larger for fixed-width fields")
        if byte_size > 0:
            self._byte_size = byte_size
        else:
            self._byte_size = {
                DataType.BOOL: 1,

                DataType.INT_8: 1,
                DataType.INT_16: 2,
                DataType.INT_32: 4,
                DataType.INT_64: 8,

                DataType.UNSIGNED_INT_8: 1,
                DataType.UNSIGNED_INT_16: 2,
                DataType.UNSIGNED_INT_32: 4,
                DataType.UNSIGNED_INT_64: 8,

                DataType.FLOAT: 4,
                DataType.DOUBLE: 8
            }[data_type]

    @property
    def name(self) -> str:
        return self._name

    @property
    def data_type(self) -> DataType:
        return self._data_type

    @property
    def size(self) -> int:
        return self._byte_size

    @property
    def signed(self) -> bool:
        return self._data_type in [
            DataType.INT_8,
            DataType.INT_16,
            DataType.INT_32,
            DataType.INT_64,

            DataType.FLOAT,
            DataType.DOUBLE
        ]