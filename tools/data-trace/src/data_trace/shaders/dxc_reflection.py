"""
Reads DXC reflection files (dxc -Fre) through dxcompiler.dll with ctypes.

Only the few COM methods needed are declared, by their position in the interface's virtual table, which must match
tools/dxc/inc/dxcapi.h and d3d12shader.h:
- IDxcUtils: 0-2 IUnknown, ..., 13 CreateReflection
- ID3D12ShaderReflection: 0-2 IUnknown, 3 GetDesc, 4 GetConstantBufferByIndex, 5 GetConstantBufferByName,
  6 GetResourceBindingDesc
- ID3D12ShaderReflectionConstantBuffer (not a COM object): 0 GetDesc
"""

import ctypes
from ctypes import POINTER, byref, c_char_p, c_long, c_size_t, c_ubyte, c_uint, c_uint16, c_uint32, c_void_p
import os
from pathlib import Path
import uuid

from data_trace.exceptions import ShaderManifestError
from data_trace.shaders.bindings import ReflectedResource, ShaderInputType


class _Guid(ctypes.Structure):
    _fields_ = [("data1", c_uint32), ("data2", c_uint16), ("data3", c_uint16), ("data4", c_ubyte * 8)]

    @classmethod
    def parse(cls, text: str) -> "_Guid":
        return cls.from_buffer_copy(uuid.UUID(text).bytes_le)


class _DxcBuffer(ctypes.Structure):
    _fields_ = [("pointer", c_void_p), ("size", c_size_t), ("encoding", c_uint32)]


_SHADER_DESC_COUNTERS = [
    "flags", "constant_buffers", "bound_resources", "input_parameters", "output_parameters", "instruction_count",
    "temp_register_count", "temp_array_count", "def_count", "dcl_count", "texture_normal_instructions",
    "texture_load_instructions", "texture_comp_instructions", "texture_bias_instructions",
    "texture_gradient_instructions", "float_instruction_count", "int_instruction_count", "uint_instruction_count",
    "static_flow_control_count", "dynamic_flow_control_count", "macro_instruction_count", "array_instruction_count",
    "cut_instruction_count", "emit_instruction_count", "gs_output_topology", "gs_max_output_vertex_count",
    "input_primitive", "patch_constant_parameters", "gs_instance_count", "control_points", "hs_output_primitive",
    "hs_partitioning", "tessellator_domain", "barrier_instructions", "interlocked_instructions",
    "texture_store_instructions",
]


class _ShaderDesc(ctypes.Structure):  # D3D12_SHADER_DESC
    _fields_ = [("version", c_uint), ("creator", c_char_p)] + [(name, c_uint) for name in _SHADER_DESC_COUNTERS]


class _InputBindDesc(ctypes.Structure):  # D3D12_SHADER_INPUT_BIND_DESC
    _fields_ = [
        ("name", c_char_p),
        ("type", c_uint),
        ("bind_point", c_uint),
        ("bind_count", c_uint),
        ("flags", c_uint),
        ("return_type", c_uint),
        ("dimension", c_uint),
        ("sample_count", c_uint),
        ("space", c_uint),
        ("id", c_uint),
    ]


class _BufferDesc(ctypes.Structure):  # D3D12_SHADER_BUFFER_DESC
    _fields_ = [("name", c_char_p), ("type", c_uint), ("variables", c_uint), ("size", c_uint), ("flags", c_uint)]


_CLSID_DXC_UTILS = _Guid.parse("6245d6af-66e0-48fd-80b4-4d271796748c")
_IID_DXC_UTILS = _Guid.parse("4605c4cb-2019-492a-ada4-65f20bb7d67f")
_IID_SHADER_REFLECTION = _Guid.parse("5a58797d-a72c-478d-8ba2-efc6b0efe88e")

_RELEASE = 2
_UTILS_CREATE_REFLECTION = 13
_REFLECTION_GET_DESC = 3
_REFLECTION_GET_CONSTANT_BUFFER_BY_NAME = 5
_REFLECTION_GET_RESOURCE_BINDING_DESC = 6
_CONSTANT_BUFFER_GET_DESC = 0


def _method(instance: int, index: int, result_type, *argument_types):
    table = ctypes.cast(c_void_p(instance), POINTER(POINTER(c_void_p))).contents
    prototype = ctypes.WINFUNCTYPE(result_type, c_void_p, *argument_types)
    function = prototype(table[index])
    return lambda *arguments: function(instance, *arguments)


def _check(result: int, operation: str):
    if result < 0:
        raise ShaderManifestError(f"{operation} failed (HRESULT 0x{result & 0xFFFFFFFF:08X})")


class DxcReflection:
    """
    Opens dxcompiler.dll from `dxc_directory` and reads the resources bound by register from reflection files.
    Use as a context manager, so the DXC utility object is released.
    """

    def __init__(self, dxc_directory: Path):
        if os.name != "nt":
            raise ShaderManifestError("Reading DXC reflection requires Windows (dxcompiler.dll)")
        library_path = Path(dxc_directory) / "dxcompiler.dll"
        if not library_path.is_file():
            raise FileNotFoundError(f"Could not find dxcompiler.dll in {dxc_directory}")

        self._library = ctypes.WinDLL(str(library_path))
        create_instance = self._library.DxcCreateInstance
        create_instance.argtypes = [POINTER(_Guid), POINTER(_Guid), POINTER(c_void_p)]
        create_instance.restype = c_long

        utils = c_void_p()
        _check(create_instance(byref(_CLSID_DXC_UTILS), byref(_IID_DXC_UTILS), byref(utils)), "DxcCreateInstance")
        self._utils: int | None = utils.value

    def close(self):
        if self._utils:
            _method(self._utils, _RELEASE, c_uint)()
            self._utils = None

    def __enter__(self) -> "DxcReflection":
        return self

    def __exit__(self, *exception_info):
        self.close()

    def read(self, reflection_path: Path) -> list[ReflectedResource]:
        if not self._utils:
            raise ShaderManifestError("DxcReflection used after close()")
        data = Path(reflection_path).read_bytes()
        storage = ctypes.create_string_buffer(data, len(data))
        source = _DxcBuffer(ctypes.cast(storage, c_void_p), len(data), 0)

        reflection = c_void_p()
        create_reflection = _method(
            self._utils, _UTILS_CREATE_REFLECTION, c_long, POINTER(_DxcBuffer), POINTER(_Guid), POINTER(c_void_p)
        )
        _check(
            create_reflection(byref(source), byref(_IID_SHADER_REFLECTION), byref(reflection)),
            f"CreateReflection for {reflection_path}",
        )
        try:
            return self._resources(reflection.value, reflection_path)
        finally:
            _method(reflection.value, _RELEASE, c_uint)()

    def _resources(self, reflection: int, reflection_path: Path) -> list[ReflectedResource]:
        description = _ShaderDesc()
        _check(
            _method(reflection, _REFLECTION_GET_DESC, c_long, POINTER(_ShaderDesc))(byref(description)),
            f"GetDesc for {reflection_path}",
        )

        get_binding = _method(reflection, _REFLECTION_GET_RESOURCE_BINDING_DESC, c_long, c_uint, POINTER(_InputBindDesc))
        get_constant_buffer = _method(reflection, _REFLECTION_GET_CONSTANT_BUFFER_BY_NAME, c_void_p, c_char_p)

        resources = []
        for index in range(description.bound_resources):
            binding = _InputBindDesc()
            _check(get_binding(index, byref(binding)), f"GetResourceBindingDesc({index}) for {reflection_path}")
            name = (binding.name or b"").decode("utf-8", errors="replace")

            size = 0
            if binding.type == ShaderInputType.CBUFFER:
                constant_buffer = get_constant_buffer(binding.name)
                buffer_description = _BufferDesc()
                result = (
                    _method(constant_buffer, _CONSTANT_BUFFER_GET_DESC, c_long, POINTER(_BufferDesc))(
                        byref(buffer_description)
                    )
                    if constant_buffer
                    else -1
                )
                _check(result, f"constant buffer size of '{name}' in {reflection_path}")
                size = buffer_description.size

            resources.append(
                ReflectedResource(
                    name=name,
                    input_type=binding.type,
                    register=binding.bind_point,
                    count=binding.bind_count,
                    space=binding.space,
                    size=size,
                )
            )
        return resources
