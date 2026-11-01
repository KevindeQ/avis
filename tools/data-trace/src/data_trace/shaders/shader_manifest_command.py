from argparse import ArgumentParser
import logging
from pathlib import Path

from data_trace.command import Command
from data_trace.shaders.dxc_reflection import DxcReflection
from data_trace.shaders.manifest import build_manifest, read_shader_list, render_manifest, write_if_changed

logger = logging.getLogger()


class ShaderManifestCommand(Command):
    """
    Writes the shader manifest for a set of compiled shaders: id, file, stage, entry, defines, xxh3 hash, and the
    bindings read from DXC reflection files (design doc 20 §8). Run by avis_add_shaders after compiling.
    """

    @staticmethod
    def name() -> str:
        return "shader-manifest"

    @staticmethod
    def init_arguments(arg_parser: ArgumentParser):
        arg_parser.add_argument("shader_list", help="shader list written by the build (avis_shader_list: 1)")
        arg_parser.add_argument("--output", required=True, help="manifest.yaml to write")
        arg_parser.add_argument("--dxc", required=True, help="directory that contains dxcompiler.dll")

    def run(self, settings) -> int:
        list_path = Path(settings.shader_list)
        if not list_path.is_file():
            raise FileNotFoundError(f"Could not find shader list: {list_path}")
        output_path = Path(settings.output)

        entries = read_shader_list(list_path)
        with DxcReflection(Path(settings.dxc)) as reflection:
            manifest = build_manifest(entries, output_path, reflection.read)

        if write_if_changed(output_path, render_manifest(manifest)):
            logger.info("Wrote %s (%d shaders)", output_path, len(entries))
        else:
            logger.info("%s is up to date (%d shaders)", output_path, len(entries))
        return 0
