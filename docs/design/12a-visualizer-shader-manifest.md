# 12a — Visualizer: shader manifest parser (draft for review)

> **Status: draft.** The graphics layer loads a shader library from a `graphics::shader_manifest` and leaves the manifest's origin to the application ([18 §3–4](18-graphics-shaders-and-pipelines.md#3-graphicsshader_manifesth), D-73). The Visualizer's shaders are built by `avis_add_shaders`, which writes `manifest.yaml` next to the blobs ([13 §3](13-build-and-quality.md#3-shader-build-pipeline)). This document designs the Visualizer's parser for that file and how shader loading fits into its startup. Once decided, it's folded into [12](12-visualizer.md) and [11 §13](11-renderer.md#13-shaders-location-and-build).

## 0. Summary

- **What:** a pure function `parse_shader_manifest(yaml_text, source_name)` that turns the text of `manifest.yaml` into a `graphics::shader_manifest`, or a parse error with `file:line:column` and the key path.
- **Where:** `apps/visualizer/rendering/shader_manifest_parser.{h,cpp}`, namespace `avis::visualizer::rendering`. It's application code: the library has no parser for the build's format.
- **What it doesn't do:** read files, check hashes, or create the library. The startup sequence reads the file with the IO layer, calls the parser, and hands the manifest to `graphics::load_shader_library`, which reads the blobs and checks their hashes (§5).

## 1. Code placement

| Feature | Namespace | CMake target | Location |
|---|---|---|---|
| Manifest parser | `avis::visualizer::rendering` | `AVIS_VISUALIZER` (also compiled into `AVIS_TESTS`, §6) | `apps/visualizer/rendering/shader_manifest_parser.{h,cpp}` |
| Shader loading at startup | `avis::visualizer::rendering` | `AVIS_VISUALIZER` | `apps/visualizer/rendering/frame_loop.cpp` (§5) |

Dependencies: `AVIS_LIB` (for `avis::error`, `graphics::shader_manifest`, `graphics::shader_stage`, [18](18-graphics-shaders-and-pipelines.md)) and yaml-cpp, which the Visualizer already links for `visualizer.yaml` and scenarios.

## 2. The file format (`avis_shader_manifest: 1`)

Written by `dt shader-manifest` from `tools/data-trace`, run by `avis_add_shaders` (13 §3). Format 2 adds a `bindings` key per entry ([18 §6](18-graphics-shaders-and-pipelines.md#6-reflection-data-sources)), still to be added here (18 §13); the example is the one in [18 §2](18-graphics-shaders-and-pipelines.md#2-graphicsshader_libraryh):

```yaml
avis_shader_manifest: 1
shaders:
  - id: lighting.main.cs
    file: lighting.main.cs.cso
    stage: cs
    entry: main
    defines: []
    xxh3: "5f0c1e2a9b3d7a41"
  - id: gbuffer.vertex_main.alpha_mask.vs
    file: gbuffer.vertex_main.alpha_mask.vs.cso
    stage: vs
    entry: vertex_main
    defines: [ALPHA_MASK]
    xxh3: "0b9e44d1c37a8f02"
```

| Key | Required | Type and rule | Maps to `graphics::shader_manifest_entry` |
|---|---|---|---|
| `avis_shader_manifest` (top level) | yes | integer, must be `1` | — (checked) |
| `shaders` (top level) | yes | sequence, may be empty | `shader_manifest::entries`, in file order |
| `id` | yes | non-empty string, unique in the file | `id` |
| `file` | yes | relative path: no root, no drive, no `..` component | `blob` |
| `stage` | yes | `vs`, `ps` or `cs` (the DXC target prefixes of 13 §3) | `stage`: `vertex`, `pixel`, `compute` |
| `entry` | no | string | `entry` (informational) |
| `defines` | no | sequence of strings | `defines` (informational) |
| `xxh3` | yes (Q2) | string of exactly 16 hexadecimal digits, case-insensitive | `content_hash` |

- **Unknown keys** are errors, at the top level, in entries and in `bindings` (Q1, decided B for task 11.1): the manifest is generated, so an unknown key means the writer and the parser drifted apart. A format 1 entry with `bindings` is one too. An `avis_shader_manifest` value other than 1 or 2 is an error: the format changed incompatibly.
- **`file` is checked for path traversal** because the loader joins it to the shader directory: an entry must not reach outside it, whatever wrote the manifest.
- **Duplicate ids** are reported here with both line numbers. `graphics::shader_library::create` would reject them too (`duplicate_shader_id`), but without positions in the file.
- The parser doesn't check that ids follow the `<name>.<entry>[.<permutation>].<stage>` convention or that the file name matches the id; the build script guarantees both, and nothing at runtime depends on them.

## 3. Interface: `rendering/shader_manifest_parser.h`

```cpp
namespace avis::visualizer::rendering
{
    // Parses the text of a manifest.yaml written by avis_add_shaders (format 1). `source_name` prefixes error
    // messages (normally the file path). Errors are error_domain::parse:
    //   "<source_name>:<line>:<column>: <key path>: <message>"
    // for example "data/shaders/visualizer/manifest.yaml:14:12: shaders[1].stage: expected vs, ps or cs, got 'gs'".
    // Pure: no file access, no logging, no global state; safe on any thread.
    result<graphics::shader_manifest> parse_shader_manifest(std::string_view yaml_text,
                                                            std::string_view source_name = {});
}
```

- **One function, no class.** The parser has no state and no options; the Visualizer has one manifest format to read.
- **First error wins.** Parsing stops at the first problem and returns it. Manifests are generated, so a malformed one means a broken build, and one precise message is more useful than a list.
- **Error positions** follow the pattern of `runtime_configuration::parse` ([14 §3](14-library-runtime.md#3-configuration)): yaml-cpp's 0-based marks become 1-based `line:column`, with `0:0` when yaml-cpp has no position. Key paths use `shaders[<index>].<key>`, as scenario errors use dotted paths ([04](04-scenario-and-dataset.md)).

## 4. Implementation notes

- `YAML::Load` inside `try`; `YAML::ParserException` becomes the error with its mark. Every `as<T>()` goes through small typed readers (`read_string`, `read_integer`, `read_string_sequence`) that return `result` and carry the key path, like the `section_reader` in `lib/avis/runtime_configuration.cpp`.
- The hash string is parsed with `std::from_chars` (base 16) after a length check; `"0x"` prefixes are rejected, since the build never writes them.
- Path checks use `std::filesystem::path(text)`: reject `has_root_path()`, and any element equal to `..`. The text is UTF-8 (`std::filesystem::path(std::u8string_view)`), so non-ASCII file names survive on Windows.
- Ids are checked for uniqueness with an `std::unordered_map<std::string_view, int line>` over the entries.
- No logging: the caller decides how to present the error (§5).

## 5. Shader loading at Visualizer startup

The renderer's startup, after the RHI exists and before pipelines are created ([12 §3.2](12-visualizer.md#32-setup-sequence) step 3, [11 §5](11-renderer.md)):

```cpp
// apps/visualizer/rendering/frame_loop.cpp (sketch)
auto load_shaders(const std::filesystem::path& directory)            // render.shader_directory, resolved
{
    const std::filesystem::path manifest_path = directory / "manifest.yaml";
    return io::read_file(manifest_path)                                 // IO layer, default IO context
         | stdexec::continues_on(stdexec::get_parallel_scheduler())
         | stdexec::let_value(
               [directory, manifest_path](std::vector<std::byte>& bytes)
               {
                   const std::string_view text{ reinterpret_cast<const char*>(bytes.data()), bytes.size() };
                   return just_result(parse_shader_manifest(text, manifest_path.generic_string()))   // set_error on failure
                        | stdexec::let_value(
                              [directory](graphics::shader_manifest& manifest)
                              {
                                  return graphics::load_shader_library(std::move(manifest), directory);
                              });
               });
}
```

1. **Resolve the directory:** `render.shader_directory` from `visualizer.yaml`, or `<executable directory>/data/shaders/visualizer` when empty ([11 §14](11-renderer.md#14-configuration-visualizeryaml--render)).
2. **Read** `manifest.yaml` with `io::read_file`.
3. **Parse** on a parallel worker with `parse_shader_manifest`, passing the path as `source_name`.
4. **Load** with `graphics::load_shader_library(manifest, directory)` ([18 §4](18-graphics-shaders-and-pipelines.md#4-graphicsshader_loadingh)): it reads every blob relative to the directory, checks each `xxh3`, and calls `graphics::shader_library::create`.
5. **Create pipelines** from the library: the renderer's startup `bulk` over the pipeline descriptions, through `graphics::pipeline_cache` ([18 §11](18-graphics-shaders-and-pipelines.md#11-graphicspipeline_cacheh)).

`just_result` is the small Visualizer helper that turns a `result<T>` into a sender completing with `set_value(T)` or `set_error(avis::error)`.

**Errors.** Any failure in steps 2–5 is a startup error. The Visualizer shows a modal dialog with the message and the log path, then exits, like the capability failure ([12 §7](12-visualizer.md#7-error-presentation)):

| Failure | Message shape |
|---|---|
| Manifest missing or unreadable | IO error with the path: *"manifest.yaml not found in <directory>; was the Visualizer built and deployed with its shaders?"* |
| Malformed manifest | Parse error: *"<path>:14:12: shaders[1].stage: expected vs, ps or cs, got 'gs'"* |
| Blob missing or hash mismatch | From `load_shader_library`, naming the entry's id and file |
| Duplicate id or hash collision | From the parser (with lines) or `shader_library::create` (`duplicate_shader_id`) |
| Missing shader for a pipeline | `shader_not_found` from `graphics::pipeline_cache` ([18 §11](18-graphics-shaders-and-pipelines.md#11-graphicspipeline_cacheh)), naming the pipeline's debug name |

## 6. Testing

The parser is compiled into `AVIS_TESTS` as well as the Visualizer (`target_sources(AVIS_TESTS PRIVATE ${CMAKE_SOURCE_DIR}/apps/visualizer/rendering/shader_manifest_parser.cpp)`), because it only depends on `AVIS_LIB` and yaml-cpp. Tests in `tests/visualizer/test_shader_manifest_parser.cpp`, tagged `[visualizer]`:

- the two-entry example of §2 parses into the expected entries (stages, hashes, defines, order);
- an empty `shaders` sequence gives an empty manifest;
- each rule of §2 fails with the expected key path and position: wrong or missing version, missing `id`/`file`/`stage`/`xxh3`, a bad stage, a 15-digit or non-hex hash, an absolute path, a `..` path, a duplicate id (both lines in the message);
- unknown keys are ignored;
- malformed YAML reports yaml-cpp's position;
- **build round trip:** a test reads the manifest the build actually wrote (`<bin>/data/shaders/visualizer/manifest.yaml`, path passed in by CTest) and parses it, so the build's manifest writer and this parser can't drift apart. It's skipped with a message when the Visualizer's shaders weren't built.

## 7. Open questions

### Q1. Unknown keys
**Decided: B** (task 11.1). The build writes the file, and the round-trip test keeps writer and parser in step; additive changes bump the format.

- **A (recommended): ignore them** (§2). The build script can add fields (a source path, a compile timestamp) without breaking deployed Visualizers within format 1; incompatible changes bump the version.
- **B: reject them.** Catches typos in hand-written manifests, but manifests are generated, and every additive change would need a version bump.

### Q2. Is `xxh3` required?
**Decided: A** (task 11.1). Format 2 entries also require their `bindings`.

- **A (recommended): required.** The build always writes it, and the loader's integrity check is the only protection against a stale or half-copied deployment.
- **B: optional,** checked when present (the graphics structure allows that). Convenient for hand-written test manifests, but a build script regression that drops hashes would go unnoticed. Tests can build a `graphics::shader_manifest` directly instead of writing YAML.

### Q3. Where the parser lives if another application needs it
- **A (recommended for now): in the Visualizer** (§1). It's the only application with shaders.
- **B: a small shared application library** (for example `apps/common/`) once a second application (a tool, an offline renderer) loads the build's shaders. Moving it then is mechanical; the library stays free of the format either way.
