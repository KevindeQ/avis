from dataclasses import dataclass, field
from enum import IntEnum
from typing import Any, Iterable

from data_trace.exceptions import ShaderManifestError


class ShaderInputType(IntEnum):
    """
    D3D_SHADER_INPUT_TYPE, as reported by ID3D12ShaderReflection::GetResourceBindingDesc
    """

    CBUFFER = 0
    TBUFFER = 1
    TEXTURE = 2
    SAMPLER = 3
    UAV_RWTYPED = 4
    STRUCTURED = 5
    UAV_RWSTRUCTURED = 6
    BYTEADDRESS = 7
    UAV_RWBYTEADDRESS = 8
    UAV_APPEND_STRUCTURED = 9
    UAV_CONSUME_STRUCTURED = 10
    UAV_RWSTRUCTURED_WITH_COUNTER = 11
    RTACCELERATIONSTRUCTURE = 12
    UAV_FEEDBACKTEXTURE = 13


@dataclass(frozen=True)
class ReflectedResource:
    """
    One resource a shader binds by register (one D3D12_SHADER_INPUT_BIND_DESC)
    """

    name: str
    input_type: int
    register: int
    count: int
    space: int
    size: int = 0  # bytes; constant buffers only


@dataclass(frozen=True)
class ConstantBufferBinding:
    register: int
    space: int
    size: int


@dataclass(frozen=True)
class BufferBinding:
    kind: str  # "shader_resource" or "unordered_access"
    register: int
    space: int


@dataclass(frozen=True)
class SamplerBinding:
    register: int
    space: int


@dataclass
class ShaderBindings:
    """
    What a shader binds by register; mirrors graphics::shader_bindings (design doc 20 §5)
    """

    constant_buffers: list[ConstantBufferBinding] = field(default_factory=list)
    buffers: list[BufferBinding] = field(default_factory=list)
    samplers: list[SamplerBinding] = field(default_factory=list)

    def to_manifest(self) -> dict[str, Any]:
        def order(binding) -> tuple[int, int]:
            return (binding.space, binding.register)

        return {
            "constant_buffers": [
                {"register": b.register, "space": b.space, "size": b.size}
                for b in sorted(self.constant_buffers, key=order)
            ],
            "buffers": [
                {"kind": b.kind, "register": b.register, "space": b.space}
                for b in sorted(self.buffers, key=lambda b: (b.kind, b.space, b.register))
            ],
            "samplers": [{"register": b.register, "space": b.space} for b in sorted(self.samplers, key=order)],
        }


_SHADER_RESOURCE_BUFFERS = {ShaderInputType.STRUCTURED, ShaderInputType.BYTEADDRESS}
_UNORDERED_ACCESS_BUFFERS = {ShaderInputType.UAV_RWSTRUCTURED, ShaderInputType.UAV_RWBYTEADDRESS}

# Bindings no root signature can provide: root signatures have no descriptor tables (design doc 20 §3)
_UNSUPPORTED = {
    ShaderInputType.TBUFFER: "tbuffer",
    ShaderInputType.TEXTURE: "texture",
    ShaderInputType.UAV_RWTYPED: "typed UAV",
    ShaderInputType.UAV_APPEND_STRUCTURED: "append buffer",
    ShaderInputType.UAV_CONSUME_STRUCTURED: "consume buffer",
    ShaderInputType.UAV_RWSTRUCTURED_WITH_COUNTER: "structured buffer with counter",
    ShaderInputType.RTACCELERATIONSTRUCTURE: "acceleration structure",
    ShaderInputType.UAV_FEEDBACKTEXTURE: "feedback texture",
}


def _register_name(resource: ReflectedResource) -> str:
    if resource.input_type == ShaderInputType.CBUFFER:
        letter = "b"
    elif resource.input_type == ShaderInputType.SAMPLER:
        letter = "s"
    elif resource.input_type in (
        ShaderInputType.UAV_RWTYPED,
        ShaderInputType.UAV_RWSTRUCTURED,
        ShaderInputType.UAV_RWBYTEADDRESS,
        ShaderInputType.UAV_APPEND_STRUCTURED,
        ShaderInputType.UAV_CONSUME_STRUCTURED,
        ShaderInputType.UAV_RWSTRUCTURED_WITH_COUNTER,
        ShaderInputType.UAV_FEEDBACKTEXTURE,
    ):
        letter = "u"
    else:
        letter = "t"
    return f"{letter}{resource.register} space{resource.space}"


def bindings_from_reflection(shader_id: str, resources: Iterable[ReflectedResource]) -> ShaderBindings:
    """
    Maps reflected resources onto shader bindings. Resources accessed through ResourceDescriptorHeap or
    SamplerDescriptorHeap aren't reported by reflection and need nothing. Raises ShaderManifestError listing every
    binding that no root signature could provide.
    """
    bindings = ShaderBindings()
    problems: list[str] = []

    for resource in resources:
        where = f"'{resource.name}' ({_register_name(resource)})"
        if resource.input_type in _UNSUPPORTED:
            problems.append(
                f"{where}: a {_UNSUPPORTED[ShaderInputType(resource.input_type)]} bound by register can't be provided "
                "by a root signature; access it through the descriptor heap"
            )
            continue
        if resource.count != 1:
            problems.append(f"{where}: arrays can't be bound through a root signature; use the descriptor heap")
            continue

        if resource.input_type == ShaderInputType.CBUFFER:
            bindings.constant_buffers.append(ConstantBufferBinding(resource.register, resource.space, resource.size))
        elif resource.input_type in _SHADER_RESOURCE_BUFFERS:
            bindings.buffers.append(BufferBinding("shader_resource", resource.register, resource.space))
        elif resource.input_type in _UNORDERED_ACCESS_BUFFERS:
            bindings.buffers.append(BufferBinding("unordered_access", resource.register, resource.space))
        elif resource.input_type == ShaderInputType.SAMPLER:
            bindings.samplers.append(SamplerBinding(resource.register, resource.space))
        else:
            problems.append(f"{where}: unknown resource type {resource.input_type}")

    if problems:
        raise ShaderManifestError(f"Shader {shader_id}: " + "; ".join(problems))
    return bindings
