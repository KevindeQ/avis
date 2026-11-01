"""
End-to-end tests with the DXC in tools/dxc: compile probe shaders, then read their reflection files.
Skipped when not on Windows or when tools/dxc isn't there.
"""

import os
from pathlib import Path
import subprocess

import pytest
import yaml

from data_trace.exceptions import ShaderManifestError
from data_trace.shaders.bindings import bindings_from_reflection
from data_trace.shaders.dxc_reflection import DxcReflection
from data_trace.shaders.manifest import build_manifest, read_shader_list, render_manifest

DXC_DIRECTORY = Path(__file__).resolve().parents[3] / "dxc" / "bin" / "x64"

pytestmark = pytest.mark.skipif(
    os.name != "nt" or not (DXC_DIRECTORY / "dxc.exe").is_file(), reason="needs Windows and tools/dxc"
)

PROBE_COMPUTE = """
struct draw_constants { uint texture_index; uint b; uint c; uint d; };
ConstantBuffer<draw_constants> g_draw : register(b0);
cbuffer pass_constants : register(b1, space0) { float4x4 view_projection; };
StructuredBuffer<float4> g_positions : register(t0, space1);
RWByteAddressBuffer g_output : register(u0);
SamplerState g_linear : register(s1);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Texture2D<float4> image = ResourceDescriptorHeap[g_draw.texture_index];   // bindless: not reflected
    float4 value = image.SampleLevel(g_linear, float2(0, 0), 0) + g_positions[id.x] + view_projection[0];
    g_output.Store4(id.x * 16, asuint(value));
}
"""

PROBE_TEXTURE_BY_REGISTER = """
Texture2D<float4> g_albedo : register(t3);
SamplerState g_linear : register(s0);
float4 main(float2 uv : TEXCOORD0) : SV_Target { return g_albedo.Sample(g_linear, uv); }
"""


def compile_probe(directory: Path, name: str, source: str, target: str) -> tuple[Path, Path]:
    source_path = directory / f"{name}.hlsl"
    source_path.write_text(source, encoding="utf-8")
    bytecode = directory / f"{name}.cso"
    reflection = directory / "reflection" / f"{name}.ref"
    reflection.parent.mkdir(exist_ok=True)
    subprocess.run(
        [str(DXC_DIRECTORY / "dxc.exe"), "-T", target, "-E", "main", "-HV", "2021", "-O3", "-Qstrip_reflect",
         "-Fo", str(bytecode), "-Fre", str(reflection), str(source_path)],
        check=True,
        capture_output=True,
    )
    return bytecode, reflection


def test_reads_register_bindings(tmp_path):
    _, reflection_path = compile_probe(tmp_path, "probe.main.cs", PROBE_COMPUTE, "cs_6_6")

    with DxcReflection(DXC_DIRECTORY) as reflection:
        bindings = bindings_from_reflection("probe.main.cs", reflection.read(reflection_path))

    assert bindings.to_manifest() == {
        "constant_buffers": [{"register": 0, "space": 0, "size": 16}, {"register": 1, "space": 0, "size": 64}],
        "buffers": [
            {"kind": "shader_resource", "register": 0, "space": 1},
            {"kind": "unordered_access", "register": 0, "space": 0},
        ],
        "samplers": [{"register": 1, "space": 0}],
    }


def test_texture_bound_by_register_is_rejected(tmp_path):
    _, reflection_path = compile_probe(tmp_path, "probe.main.ps", PROBE_TEXTURE_BY_REGISTER, "ps_6_6")

    with DxcReflection(DXC_DIRECTORY) as reflection:
        resources = reflection.read(reflection_path)
    with pytest.raises(ShaderManifestError, match=r"'g_albedo' \(t3 space0\): a texture bound by register"):
        bindings_from_reflection("probe.main.ps", resources)


def test_command_pipeline_writes_manifest(tmp_path):
    compile_probe(tmp_path, "probe.main.cs", PROBE_COMPUTE, "cs_6_6")
    list_path = tmp_path / "shaders.yaml"
    list_path.write_text(
        yaml.safe_dump(
            {
                "avis_shader_list": 1,
                "shaders": [
                    {"id": "probe.main.cs", "stage": "cs", "entry": "main", "defines": [],
                     "bytecode": "probe.main.cs.cso", "reflection": "reflection/probe.main.cs.ref"}
                ],
            }
        ),
        encoding="utf-8",
    )

    with DxcReflection(DXC_DIRECTORY) as reflection:
        manifest = build_manifest(read_shader_list(list_path), tmp_path / "manifest.yaml", reflection.read)

    parsed = yaml.safe_load(render_manifest(manifest))
    assert parsed["avis_shader_manifest"] == 2
    assert parsed["shaders"][0]["file"] == "probe.main.cs.cso"
    assert parsed["shaders"][0]["bindings"]["samplers"] == [{"register": 1, "space": 0}]
