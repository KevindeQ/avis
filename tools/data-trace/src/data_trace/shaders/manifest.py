"""
The shader list written by the build (input) and the shader manifest read by the application (output).

Shader list (`avis_shader_list: 1`), written by avis_add_shaders; relative paths are relative to the list file:

    avis_shader_list: 1
    shaders:
      - id: lighting.main.cs
        stage: cs
        entry: main
        defines: []
        bytecode: lighting.main.cs.cso
        reflection: reflection/lighting.main.cs.ref

Manifest (`avis_shader_manifest: 2`), read by the Visualizer (design docs 12a and 20 §8).
"""

from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Callable

import xxhash
import yaml

from data_trace.exceptions import ShaderManifestError
from data_trace.shaders.bindings import ReflectedResource, bindings_from_reflection

SHADER_LIST_VERSION = 1
MANIFEST_VERSION = 2
STAGES = ("vs", "ps", "cs")

MANIFEST_HEADER = "# Written by `dt shader-manifest` during the build; do not edit.\n"


@dataclass(frozen=True)
class ShaderListEntry:
    id: str
    stage: str
    entry: str
    defines: tuple[str, ...]
    bytecode: Path
    reflection: Path


def _required(item: dict, key: str, index: int, list_path: Path):
    if key not in item:
        raise ShaderManifestError(f"{list_path}: shaders[{index}].{key} is missing")
    return item[key]


def read_shader_list(list_path: Path) -> list[ShaderListEntry]:
    list_path = Path(list_path)
    with open(list_path, encoding="utf-8") as file:
        try:
            document = yaml.safe_load(file)
        except yaml.YAMLError as error:
            raise ShaderManifestError(f"{list_path}: not valid YAML: {error}") from error

    if not isinstance(document, dict) or document.get("avis_shader_list") != SHADER_LIST_VERSION:
        raise ShaderManifestError(f"{list_path}: expected 'avis_shader_list: {SHADER_LIST_VERSION}'")
    shaders = document.get("shaders")
    if not isinstance(shaders, list):
        raise ShaderManifestError(f"{list_path}: 'shaders' must be a sequence")

    base = list_path.parent
    entries = []
    for index, item in enumerate(shaders):
        if not isinstance(item, dict):
            raise ShaderManifestError(f"{list_path}: shaders[{index}] must be a mapping")
        stage = _required(item, "stage", index, list_path)
        if stage not in STAGES:
            raise ShaderManifestError(f"{list_path}: shaders[{index}].stage: expected vs, ps or cs, got '{stage}'")
        defines = item.get("defines") or []
        if not isinstance(defines, list):
            raise ShaderManifestError(f"{list_path}: shaders[{index}].defines must be a sequence")
        entries.append(
            ShaderListEntry(
                id=str(_required(item, "id", index, list_path)),
                stage=stage,
                entry=str(item.get("entry", "")),
                defines=tuple(str(define) for define in defines),
                bytecode=base / str(_required(item, "bytecode", index, list_path)),
                reflection=base / str(_required(item, "reflection", index, list_path)),
            )
        )
    return entries


def _relative_file(bytecode: Path, manifest_directory: Path, shader_id: str) -> str:
    try:
        relative = bytecode.resolve().relative_to(manifest_directory.resolve())
    except ValueError as error:
        raise ShaderManifestError(
            f"Shader {shader_id}: {bytecode} is not inside the manifest's directory {manifest_directory}"
        ) from error
    return str(PurePosixPath(*relative.parts))


def build_manifest(
    entries: list[ShaderListEntry],
    manifest_path: Path,
    reflect: Callable[[Path], list[ReflectedResource]],
) -> dict[str, Any]:
    """
    Hashes each blob (xxh3, 64 bits), reflects its bindings, and returns the manifest as a dictionary. Raises
    ShaderManifestError for a duplicate id, a blob outside the manifest's directory, or bindings no root signature
    can provide.
    """
    manifest_directory = Path(manifest_path).parent
    seen: set[str] = set()
    shaders = []
    for entry in entries:
        if entry.id in seen:
            raise ShaderManifestError(f"Duplicate shader id {entry.id}")
        seen.add(entry.id)

        if not entry.bytecode.is_file():
            raise FileNotFoundError(f"Shader {entry.id}: could not find bytecode {entry.bytecode}")
        if not entry.reflection.is_file():
            raise FileNotFoundError(f"Shader {entry.id}: could not find reflection {entry.reflection}")

        bindings = bindings_from_reflection(entry.id, reflect(entry.reflection))
        shaders.append(
            {
                "id": entry.id,
                "file": _relative_file(entry.bytecode, manifest_directory, entry.id),
                "stage": entry.stage,
                "entry": entry.entry,
                "defines": list(entry.defines),
                "xxh3": xxhash.xxh3_64_hexdigest(entry.bytecode.read_bytes()),
                "bindings": bindings.to_manifest(),
            }
        )
    return {"avis_shader_manifest": MANIFEST_VERSION, "shaders": shaders}


def render_manifest(manifest: dict[str, Any]) -> str:
    body = yaml.safe_dump(manifest, sort_keys=False, default_flow_style=None, allow_unicode=True, width=120)
    return MANIFEST_HEADER + body


def write_if_changed(path: Path, text: str) -> bool:
    """
    Writes `text` unless the file already holds it, so an unchanged manifest keeps its timestamp and doesn't
    trigger deployment steps that depend on it. Returns whether the file was written.
    """
    path = Path(path)
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)
    return True
