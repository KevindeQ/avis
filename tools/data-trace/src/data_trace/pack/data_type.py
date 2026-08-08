from enum import Enum, auto


class DataType(Enum):
    BOOL = auto()

    INT_8 = auto()
    INT_16 = auto()
    INT_32 = auto()
    INT_64 = auto()

    UNSIGNED_INT_8 = auto()
    UNSIGNED_INT_16 = auto()
    UNSIGNED_INT_32 = auto()
    UNSIGNED_INT_64 = auto()

    FLOAT = auto()
    DOUBLE = auto()