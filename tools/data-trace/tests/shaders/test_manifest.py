from pathlib import Path

import pytest
import xxhash
import yaml

from data_trace.exceptions import ShaderManifestError
from data_trace.shaders.bindings import ReflectedResource, ShaderInputType
from data_trace.shaders.manifest import (
    MANIFEST_HEADER,
    build_manifest,
    read_shader_list,
    render_manifest,
    write_if_changed,
)


def write_list(path: Path, shaders: list[dict]) -> Path:
    path.write_text(yaml.safe_dump({"avis_shader_list": 1, "shaders": shaders}), encoding="utf-8")
    return path


def make_shader(directory: Path, name: str, content: bytes = b"DXBC") -> dict:
    (directory / f"{name}.cso").write_bytes(content)
    (directory / "reflection").mkdir(exist_ok=True)
    (directory / "reflection" / f"{name}.ref").write_bytes(b"reflection")
    return {
        "id": name,
        "stage": name.rsplit(".", 1)[-1],
        "entry": "main",
        "defines": [],
        "bytecode": f"{name}.cso",
        "reflection": f"reflection/{name}.ref",
    }


def constant_buffer_reflection(path: Path) -> list[ReflectedResource]:
    return [ReflectedResource("draw", ShaderInputType.CBUFFER, register=0, count=1, space=0, size=64)]


def test_builds_manifest_with_hash_relative_file_and_bindings(tmp_path):
    entries = read_shader_list(write_list(tmp_path / "shaders.yaml", [make_shader(tmp_path, "lighting.main.cs")]))

    manifest = build_manifest(entries, tmp_path / "manifest.yaml", constant_buffer_reflection)

    assert manifest == {
        "avis_shader_manifest": 2,
        "shaders": [
            {
                "id": "lighting.main.cs",
                "file": "lighting.main.cs.cso",
                "stage": "cs",
                "entry": "main",
                "defines": [],
                "xxh3": xxhash.xxh3_64_hexdigest(b"DXBC"),
                "bindings": {
                    "constant_buffers": [{"register": 0, "space": 0, "size": 64}],
                    "buffers": [],
                    "samplers": [],
                },
            }
        ],
    }
    assert len(manifest["shaders"][0]["xxh3"]) == 16


def test_rendered_manifest_round_trips(tmp_path):
    shader = make_shader(tmp_path, "gbuffer.vertex_main.alpha_mask.vs")
    shader["defines"] = ["ALPHA_MASK"]
    entries = read_shader_list(write_list(tmp_path / "shaders.yaml", [shader]))
    manifest = build_manifest(entries, tmp_path / "manifest.yaml", constant_buffer_reflection)

    text = render_manifest(manifest)

    assert text.startswith(MANIFEST_HEADER)
    assert yaml.safe_load(text) == manifest


def test_hash_that_looks_like_a_number_stays_a_string(tmp_path):
    manifest = {"avis_shader_manifest": 2, "shaders": [{"id": "a.cs", "xxh3": "1234567890123456"}]}
    assert yaml.safe_load(render_manifest(manifest))["shaders"][0]["xxh3"] == "1234567890123456"


def test_rejects_duplicate_ids(tmp_path):
    shader = make_shader(tmp_path, "a.main.cs")
    entries = read_shader_list(write_list(tmp_path / "shaders.yaml", [shader, shader]))
    with pytest.raises(ShaderManifestError, match="Duplicate shader id a.main.cs"):
        build_manifest(entries, tmp_path / "manifest.yaml", constant_buffer_reflection)


def test_rejects_blob_outside_manifest_directory(tmp_path):
    entries = read_shader_list(write_list(tmp_path / "shaders.yaml", [make_shader(tmp_path, "a.main.cs")]))
    with pytest.raises(ShaderManifestError, match="not inside the manifest's directory"):
        build_manifest(entries, tmp_path / "deploy" / "manifest.yaml", constant_buffer_reflection)


def test_missing_reflection_file(tmp_path):
    shader = make_shader(tmp_path, "a.main.cs")
    (tmp_path / "reflection" / "a.main.cs.ref").unlink()
    entries = read_shader_list(write_list(tmp_path / "shaders.yaml", [shader]))
    with pytest.raises(FileNotFoundError, match="could not find reflection"):
        build_manifest(entries, tmp_path / "manifest.yaml", constant_buffer_reflection)


@pytest.mark.parametrize(
    "document, message",
    [
        ({"avis_shader_list": 2, "shaders": []}, "expected 'avis_shader_list: 1'"),
        ({"avis_shader_list": 1}, "'shaders' must be a sequence"),
        ({"avis_shader_list": 1, "shaders": [{"stage": "cs"}]}, r"shaders\[0\]\.id is missing"),
        (
            {"avis_shader_list": 1, "shaders": [{"id": "a", "stage": "gs", "bytecode": "a", "reflection": "b"}]},
            r"shaders\[0\]\.stage: expected vs, ps or cs, got 'gs'",
        ),
    ],
)
def test_rejects_invalid_shader_lists(tmp_path, document, message):
    path = tmp_path / "shaders.yaml"
    path.write_text(yaml.safe_dump(document), encoding="utf-8")
    with pytest.raises(ShaderManifestError, match=message):
        read_shader_list(path)


def test_write_if_changed_keeps_unchanged_file(tmp_path):
    path = tmp_path / "out" / "manifest.yaml"
    assert write_if_changed(path, "a: 1\n")
    assert not write_if_changed(path, "a: 1\n")
    assert write_if_changed(path, "a: 2\n")
    assert path.read_text(encoding="utf-8") == "a: 2\n"
