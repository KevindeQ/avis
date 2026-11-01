import pytest

from data_trace.exceptions import ShaderManifestError
from data_trace.shaders.bindings import ReflectedResource, ShaderInputType, bindings_from_reflection


def resource(name, input_type, register=0, space=0, count=1, size=0):
    return ReflectedResource(name=name, input_type=input_type, register=register, count=count, space=space, size=size)


def test_maps_every_supported_kind():
    bindings = bindings_from_reflection(
        "probe.cs",
        [
            resource("draw", ShaderInputType.CBUFFER, register=0, size=64),
            resource("positions", ShaderInputType.STRUCTURED, register=2, space=1),
            resource("raw", ShaderInputType.BYTEADDRESS, register=3),
            resource("output", ShaderInputType.UAV_RWSTRUCTURED, register=0),
            resource("raw_output", ShaderInputType.UAV_RWBYTEADDRESS, register=1),
            resource("linear", ShaderInputType.SAMPLER, register=1),
        ],
    )

    assert bindings.to_manifest() == {
        "constant_buffers": [{"register": 0, "space": 0, "size": 64}],
        "buffers": [
            {"kind": "shader_resource", "register": 3, "space": 0},
            {"kind": "shader_resource", "register": 2, "space": 1},
            {"kind": "unordered_access", "register": 0, "space": 0},
            {"kind": "unordered_access", "register": 1, "space": 0},
        ],
        "samplers": [{"register": 1, "space": 0}],
    }


def test_no_resources_gives_empty_bindings():
    assert bindings_from_reflection("bindless.cs", []).to_manifest() == {
        "constant_buffers": [],
        "buffers": [],
        "samplers": [],
    }


@pytest.mark.parametrize(
    "input_type, register_text",
    [
        (ShaderInputType.TEXTURE, "t3 space0"),
        (ShaderInputType.TBUFFER, "t3 space0"),
        (ShaderInputType.UAV_RWTYPED, "u3 space0"),
        (ShaderInputType.UAV_APPEND_STRUCTURED, "u3 space0"),
        (ShaderInputType.RTACCELERATIONSTRUCTURE, "t3 space0"),
    ],
)
def test_rejects_bindings_no_root_signature_can_provide(input_type, register_text):
    with pytest.raises(ShaderManifestError, match=f"Shader probe.ps: 'albedo' \\({register_text}\\)"):
        bindings_from_reflection("probe.ps", [resource("albedo", input_type, register=3)])


def test_rejects_arrays():
    with pytest.raises(ShaderManifestError, match="arrays can't be bound"):
        bindings_from_reflection("probe.cs", [resource("buffers", ShaderInputType.STRUCTURED, count=4)])


def test_reports_every_problem():
    with pytest.raises(ShaderManifestError) as error:
        bindings_from_reflection(
            "probe.ps",
            [resource("a", ShaderInputType.TEXTURE, register=0), resource("b", ShaderInputType.TEXTURE, register=1)],
        )
    assert "'a' (t0 space0)" in str(error.value)
    assert "'b' (t1 space0)" in str(error.value)
