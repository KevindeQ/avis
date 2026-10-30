# 18 — Graphics layer: shaders and pipelines (draft for review)

> **Status: draft.** The RHI creates root signatures from descriptions and pipelines from bytecode, and nothing else ([09 §12](09-rhi.md#12-pipelines-rhipipelineh), [09 §12.1](09-rhi.md#121-root-signatures-rhiroot_signatureh)). Everything above that is graphics-layer policy, collected here: the shader library and its manifest, the reflection data that says which registers each shader binds, the Avis binding layout, choosing a root signature per pipeline, and the pipeline cache. The motivation for root signatures as an RHI mechanism is future flexibility, for example sensor visualisation (camera images, lidar point clouds) with a binding model of its own. Frames and data transfers are in [17](17-graphics-frames-and-transfers.md), which also defines the graphics error domain. Once decided, this is folded into the graphics-layer design with 17.

## 0. Summary

**Shaders and pipelines**

- **RHI:** `create_pipeline(graphics_pipeline_description)` takes the shader bytecode as spans plus a `graphics_pipeline_state`; every call creates a PSO. No shader ids, no library, no cache.
- **`graphics::shader_library`:** an immutable map from `shader_id` (FNV-1a of `"lighting.main.cs"`) to stage and bytecode, built from `shader_blob`s.
- **`graphics::load_shader_library(manifest, directory)`:** reads the blobs a `shader_manifest` lists, checks their hashes, builds the library. The application decides where the manifest comes from (the Visualizer: [12a](12a-visualizer-shader-manifest.md)).
- **`graphics::pipeline_cache`:** resolves shader ids through a library, checks stages, picks or checks the root signature through a `root_signature_registry` (§10), and returns the existing handle for an equal description or creates the PSO through the RHI.

**Root signatures**

- **RHI** ([09 §12.1](09-rhi.md#121-root-signatures-rhiroot_signatureh)): **root signatures are handles.** `create_root_signature(root_signature_description)` returns a `root_signature_handle`. Every pipeline description names one. `set_pipeline` switches the list's root signature when the pipeline's differs. Root arguments are set through generic calls by parameter index.
- **Per-stage visibility.** Each root parameter and static sampler is visible to all stages, or only to the vertex or pixel shader (D-74), so a register can mean different data per stage and drivers can skip loading arguments a stage doesn't use.
- **Bindless stays mandatory.** Descriptions have root constants, root descriptors and static samplers, never descriptor tables, and every signature gets the "directly indexed" heap flags. A bindless index therefore means the same under every signature, so automatic views at creation, descriptor ranges and the descriptor heaps don't change.
- **Graphics layer: the Avis layout** (`graphics::binding_layout`, formerly `rhi::root_signature_layout`): the standard signature (16 root constants at `b0`, pass CBV at `b1`, view CBV at `b2`, six static samplers) and typed helpers (`push_draw_constants`, `set_pass_constants`, `set_view_constants`).
- **Graphics layer: matching.** Each shader carries a `shader_bindings` record: the registers it uses, from reflection. The renderer registers candidate signatures in order of preference in a `root_signature_registry`. The pipeline cache picks the first compatible candidate for each pipeline, or checks the one the pipeline names, once, when the pipeline is created.
- **Reflection data is extracted at build time into the shader manifest** (§6, Q3 decided A), by the `shader-manifest` command of `tools/data-trace` ([13 §3](13-build-and-quality.md#3-shader-build-pipeline)).

## 1. Code placement

| Feature | Namespace | CMake target | Location |
|---|---|---|---|
| `shader_id`, `shader_stage`, `shader_blob`, `shader_library` | `avis::graphics` | `AVIS_LIB` | `include/avis/graphics/shader_library.h`, `lib/avis/graphics/shader_library.cpp` |
| `shader_manifest`, `shader_manifest_entry` | `avis::graphics` | `AVIS_LIB` | `include/avis/graphics/shader_manifest.h` |
| `load_shader_library`, `shader_library_load_sender` | `avis::graphics` | `AVIS_LIB` | `include/avis/graphics/shader_loading.h`, `lib/avis/graphics/shader_loading.cpp` |
| `pipeline_cache`, pipeline descriptions with shader ids (uses `root_signature_registry`, §10) | `avis::graphics` | `AVIS_LIB` | `include/avis/graphics/pipeline_cache.h`, `lib/avis/graphics/pipeline_cache.cpp` |
| `binding_layout` (the Avis standard signature and helpers) | `avis::graphics` | `AVIS_LIB` | `include/avis/graphics/binding_layout.h`, `lib/avis/graphics/binding_layout.cpp` |
| `shader_bindings` | `avis::graphics` | `AVIS_LIB` | `include/avis/graphics/shader_bindings.h` |
| `root_signature_registry`, matching | `avis::graphics` | `AVIS_LIB` | `include/avis/graphics/root_signature_registry.h`, `lib/avis/graphics/root_signature_registry.cpp` |

`shader_loading` depends on the IO layer (`read_file`) and the RHI is not involved; `shader_library` and `pipeline_cache` depend on the RHI only. Errors are `graphics::error_kind` ([17 §2](17-graphics-frames-and-transfers.md#2-graphicserrorh)), including the shader kinds and the three root-signature kinds `no_compatible_root_signature`, `root_signature_incompatible` and `shader_bindings_missing`.

The RHI side (`include/avis/rhi/root_signature.h`, `lib/avis/rhi/d3d12/root_signature.{h,cpp}`) is placed in 09 §2.1 and 09 §22.1.

## 2. `graphics/shader_library.h`

```cpp
namespace avis::graphics
{
    // 64-bit FNV-1a hash of the shader id string ("lighting.main.cs"), computed at compile time
    struct shader_id
    {
        std::uint64_t hash = 0;
        friend constexpr bool operator==(shader_id, shader_id) noexcept = default;
    };

    namespace literals
    {
        consteval shader_id operator""_shader_id(const char* text, std::size_t length) noexcept;
    }

    enum class shader_stage : std::uint8_t { vertex, pixel, compute };

    struct shader_blob
    {
        std::string id;                    // "lighting.main.cs"
        shader_stage stage = shader_stage::compute;
        std::vector<std::byte> bytecode;   // DXIL; moved in, the library keeps the only copy
        std::optional<shader_bindings> bindings;   // registers used, for root signature matching (§5)
    };

    // Immutable after creation, so many workers can read it at once
    class shader_library
    {
    public:
        // Fails with invalid_argument on an empty id or bytecode, and with duplicate_shader_id on a duplicate id
        // or two ids with the same hash (the message names both). `name` is for messages only.
        static result<shader_library> create(std::vector<shader_blob> blobs, std::string name = {});

        shader_library(shader_library&&) noexcept;
        shader_library& operator=(shader_library&&) noexcept;
        ~shader_library();

        struct entry
        {
            std::string_view id;
            shader_stage stage;
            std::span<const std::byte> bytecode;
            const shader_bindings* bindings;       // null when the blob had none
        };
        std::optional<entry> find(shader_id id) const noexcept;
        std::size_t size() const noexcept;
        const std::string& name() const noexcept;
        std::uint64_t serial() const noexcept;     // unique per created library; part of pipeline cache keys (§11)
    };
}
```

- The FNV-1a id hash replaces 09's unspecified "compile-time hashed string". It's unrelated to the manifest's xxh3 blob checksum.
- **Debug check (optional):** compare the declared stage with the shader kind in the DXIL container header, to catch a mislabelled blob early.
- `shader_bindings` ([§5](#5-graphicsshader_bindingsh)) is the reflection record used to match root signatures; `shader_library.h` includes `shader_bindings.h`.

## 3. `graphics/shader_manifest.h`

The build's manifest as a structure; the application builds it from whatever source it likes:

```cpp
namespace avis::graphics
{
    struct shader_manifest_entry
    {
        std::string id;                              // "lighting.main.cs"
        shader_stage stage = shader_stage::compute;
        std::filesystem::path blob;                  // relative to the blob directory: "lighting.main.cs.cso"
        std::string entry;                           // informational: the HLSL entry point
        std::vector<std::string> defines;            // informational: the compiler defines
        std::optional<std::uint64_t> content_hash;   // xxh3 of the blob; checked when present
        std::optional<shader_bindings> bindings;     // copied into the blob; where it comes from is Q3
    };

    struct shader_manifest
    {
        std::vector<shader_manifest_entry> entries;
    };
}
```

The library offers no parser for the build's YAML format (D-73). The Visualizer parses `manifest.yaml` with its own parser ([12a](12a-visualizer-shader-manifest.md)).

## 4. `graphics/shader_loading.h`

```cpp
namespace avis::graphics
{
    // For each entry: read_file(directory / entry.blob) into a counting scope, then on a parallel worker check
    // content_hash (xxh3) where present and call shader_library::create. Fails with the IO error, with
    // shader_hash_mismatch naming the entry, or with shader_library::create's errors.
    shader_library_load_sender load_shader_library(shader_manifest manifest, std::filesystem::path directory);
    shader_library_load_sender load_shader_library(io::io_context& context, shader_manifest manifest,
                                                   std::filesystem::path directory);
}
```

- `static_assert(avis::sender_of<shader_library_load_sender, shader_library>)`.
- Doesn't need an RHI: a library can be loaded before the device exists, in parallel with device creation.
- A tool or test with bytecode in memory skips the loader and calls `shader_library::create`.

## 5. `graphics/shader_bindings.h`

What one shader uses, independent of where the data comes from (§6):

```cpp
namespace avis::graphics
{
    struct constant_buffer_binding          // cbuffer / ConstantBuffer<T> at b<register>
    {
        std::uint32_t shader_register = 0;
        std::uint32_t register_space = 0;
        std::uint32_t size = 0;             // bytes, as laid out by HLSL packing
    };

    enum class buffer_binding_kind : std::uint8_t { shader_resource, unordered_access };

    struct buffer_binding                   // a buffer bound by register (t<n> / u<n>), not through the heap
    {
        buffer_binding_kind kind = buffer_binding_kind::shader_resource;
        std::uint32_t shader_register = 0;
        std::uint32_t register_space = 0;
    };

    struct sampler_binding                  // a sampler bound by register (s<n>), not through the heap
    {
        std::uint32_t shader_register = 0;
        std::uint32_t register_space = 0;
    };

    struct shader_bindings
    {
        std::vector<constant_buffer_binding> constant_buffers;
        std::vector<buffer_binding> buffers;
        std::vector<sampler_binding> samplers;
        // Textures or typed buffers bound by register can't be matched (no descriptor tables, 09 §12.1); the source
        // reports them as an error instead of putting them here.
    };
}
```

**Where it's carried** (§2–4): `shader_manifest_entry::bindings`, `shader_blob::bindings` and `shader_library::entry::bindings`, all optional. A shader without bindings can only be used by a pipeline that names its root signature explicitly, and then without a check.

## 6. Reflection data sources

`shader_bindings` can be filled from several places. They differ in when the data is produced, what has to ship, and who can get it wrong.

| | Source | Produced | Needs at runtime | Strengths | Weaknesses |
|---|---|---|---|---|---|
| **A** | DXC reflection, extracted by the build into the manifest | build | nothing new | no runtime dependency; errors (a texture bound by register) at build time; manifest stays the single description of a shader | a small host tool in the build; manifest format change (12a parser, round-trip test) |
| **B** | DXC reflection of the loaded bytecode (`IDxcUtils::CreateReflection`) | load | `dxcompiler.dll`; reflection kept in the `.cso` | works for any bytecode, including tools and tests with shaders in memory; can't drift from the bytecode | ships the compiler DLL (tens of MB); `.cso` files larger; the graphics layer depends on `dxcapi.h` |
| **C** | Declared by hand (a sidecar file per shader, or keys in the HLSL parsed by the build) | authoring | nothing new | no tooling | can drift from the shader; only a test catches it |
| **D** | Embedded root signature (`[RootSignature(...)]` in HLSL, the `RTS0` part of the bytecode) | build | nothing new (`D3D12CreateVersionedRootSignatureDeserializer` is in `d3d12.dll`) | standard D3D12; no reflection at all | the shader author picks the layout, not the renderer; every shader repeats it; matching becomes equality |
| **E** | None: every pipeline names its signature | — | — | simplest | no check that shaders fit; mistakes show up as GPU validation errors or wrong output |

**How A works** (chosen; the build side is in [13 §3](13-build-and-quality.md#3-shader-build-pipeline)):
- `avis_add_shaders` compiles as before, keeps `-Qstrip_reflect`, and additionally writes each shader's reflection to a separate file with DXC's `-Fre`, in the build tree.
- It also writes a shader list (`avis_shader_list: 1`) naming each shader's id, stage, entry, defines, `.cso` and reflection file.
- `dt shader-manifest` (a command of the Python tool `tools/data-trace`) reads the list, opens each reflection file through `dxcompiler.dll` from `tools/dxc` with `ctypes` (`IDxcUtils::CreateReflection`, then `ID3D12ShaderReflection::GetDesc`, `GetResourceBindingDesc` and `GetConstantBufferByName` for sizes), maps the resources onto `shader_bindings`, hashes the `.cso` (xxh3), and writes the whole manifest (format 2).
- The mapping from reflection to bindings:

  | Reflected (`D3D_SHADER_INPUT_TYPE`) | Manifest |
  |---|---|
  | `CBUFFER` | `constant_buffers: {register, space, size}` |
  | `STRUCTURED`, `BYTEADDRESS` | `buffers: {kind: shader_resource, register, space}` |
  | `UAV_RWSTRUCTURED`, `UAV_RWBYTEADDRESS` | `buffers: {kind: unordered_access, register, space}` |
  | `SAMPLER` | `samplers: {register, space}` |
  | `TEXTURE`, `TBUFFER`, `UAV_RWTYPED`, append/consume/counter buffers, acceleration structures, feedback textures, and any array | build error naming the shader, resource and register: no root signature can provide them (09 §12.1) |

- Example entry:
  ```yaml
  - id: lighting.main.cs
    file: lighting.main.cs.cso
    stage: cs
    entry: main
    defines: []
    xxh3: 8e4016d2ad80d9e1
    bindings:
      constant_buffers:
      - {register: 0, space: 0, size: 64}
      buffers: []
      samplers:
      - {register: 1, space: 0}
  ```
- 12a's parser maps the key to `shader_manifest_entry::bindings`; the round-trip test keeps writer and parser in step.
- **Status:** the command and its tests exist (`tools/data-trace/src/data_trace/shaders/`, `tools/data-trace/tests/shaders/`, including probe shaders compiled with `tools/dxc`). `avis_add_shaders` and the 12a parser changes are not implemented yet.

**How B would work.** `graphics::reflect_shader(std::span<const std::byte> bytecode) -> result<shader_bindings>`, called by `load_shader_library` when an entry has no bindings, loading `dxcompiler.dll` on first use. The build must not pass `-Qstrip_reflect`. A tool that compiles shaders at runtime already ships DXC, so B costs it nothing extra.

**Combinations.** A and B produce the same structure, so they combine: A for the Visualizer's built shaders, B later for a tool that compiles at runtime. E is always available per pipeline (name the signature) and is the fallback for shaders without bindings.

## 7. Root signatures: what changed

| Before | After |
|---|---|
| One root signature, built by the RHI from `rhi::root_signature_layout` | Any number, created from descriptions by whoever needs them; the Avis one is built by the graphics layer |
| The RHI sets the root signature on every acquired list | `set_pipeline` sets it when it changes; a fresh list has none bound |
| `push_constants`, `set_pass_constants`, `set_view_constants` (fixed parameters 0, 1, 2) | Generic `set_root_constants`, `set_root_constant_buffer`, `set_root_shader_resource`, `set_root_unordered_access`; the named ones become graphics-layer helpers |
| Pipeline descriptions without a root signature | `root_signature` field in both RHI descriptions; optional in the graphics layer's (matched when absent) |
| `bindless.hlsli` mirrors `rhi::root_signature_layout` | Mirrors `graphics::binding_layout` |

Unchanged: descriptor heaps and their layout, automatic persistent views, descriptor ranges, the null descriptor at index 0, the three command signatures.

## 8. RHI mechanism

Designed in [09 §12.1](09-rhi.md#121-root-signatures-rhiroot_signatureh). What the graphics layer relies on:
- `create_root_signature(root_signature_description)` returns a `root_signature_handle`; descriptions hold root constants, root CBVs/SRVs/UAVs and static samplers, each with a `shader_visibility` (`all`, `vertex`, `pixel`), and never descriptor tables.
- Every signature allows `ResourceDescriptorHeap` / `SamplerDescriptorHeap` (bindless is mandatory), so shaders need no binding for heap access.
- Each RHI pipeline description names its root signature; `set_pipeline` switches it when it differs, after which every root argument is undefined until set again.
- Root arguments are set by parameter index (`set_root_constants`, `set_root_constant_buffer`, `set_root_shader_resource`, `set_root_unordered_access`).
- The RHI doesn't deduplicate signatures; root signatures and pipelines live as long as the RHI.

**Private side.** In the RHI documents: the root signature record in [09 §21.3](09-rhi.md#213-d3d12resource_recordsh), creation and the command signatures in [09 §22.3–22.4](09-rhi.md#223-d3d12root_signatureh), switching and the root-call checks in [09 §24.3](09-rhi.md#243-recording).

## 9. `graphics/binding_layout.h`

The Avis standard binding model, unchanged in content from 09 §12.1, now owned by the graphics layer:

```cpp
namespace avis::graphics::binding_layout
{
    inline constexpr std::uint32_t draw_constants_parameter = 0;   // 32-bit constants, b0 space0
    inline constexpr std::uint32_t draw_constant_count = 16;
    inline constexpr std::uint32_t pass_constants_parameter = 1;   // root CBV, b1 space0
    inline constexpr std::uint32_t view_constants_parameter = 2;   // root CBV, b2 space0
    inline constexpr std::uint32_t draw_constants_register = 0;
    inline constexpr std::uint32_t pass_constants_register = 1;
    inline constexpr std::uint32_t view_constants_register = 2;

    enum class static_sampler : std::uint32_t                        // register s<n>, space0
    {
        point_clamp = 0,
        linear_clamp = 1,
        linear_wrap = 2,
        anisotropic_wrap = 3,
        shadow_comparison = 4,                                       // GREATER_EQUAL, reverse-Z, linear
        point_wrap = 5
    };

    // The description above, for root_signature_registry::add (static storage, valid for the program's lifetime)
    rhi::root_signature_description standard_description() noexcept;

    // Typed helpers over the generic RHI calls, for pipelines on the standard signature. They take a recorder, the
    // type passes get from their context (D-92, 10 §3.1); a command_list is one too, since it derives from it.
    template<class Constants, rhi::queue_type Queue>
    void push_draw_constants(rhi::command_recorder<Queue>& recorder, const Constants& constants)
    {
        static_assert(sizeof(Constants) <= draw_constant_count * 4);
        recorder.set_root_constants(draw_constants_parameter, constants);
    }
    template<rhi::queue_type Queue>
    void set_pass_constants(rhi::command_recorder<Queue>& recorder, rhi::gpu_address address);
    template<rhi::queue_type Queue>
    void set_view_constants(rhi::command_recorder<Queue>& recorder, rhi::gpu_address address);
}
```

- **Visibility:** every parameter and static sampler of the standard description is `all`. The draw, pass and view constants are read by both the vertex and the pixel shader (instance and vertex data, material index, camera), compute passes use the same signature, and samplers are used by pixel and compute shaders. A binding model with stage-specific data (a sensor visualisation, for example) can limit its parameters.
- The compile-time size check that `command_list::push_constants<T>` had moves here; the RHI's typed `set_root_constants` checks at runtime in debug builds, since it doesn't know the layout at compile time.
- `bindless.hlsli` in the Visualizer mirrors these constants, and the probe-shader test compares against them ([11 §13](11-renderer.md#13-shaders-location-and-build)).
- A second binding model (sensor rendering, a tool) gets its own header of the same shape and registers its description next to the standard one.

## 10. `graphics/root_signature_registry.h`

```cpp
namespace avis::graphics
{
    class root_signature_registry
    {
    public:
        explicit root_signature_registry(rhi::pipelines pipelines);   // the RHI's pipelines view (09 §4.3)

        // Creates the RHI root signature and appends it to the candidates; the description is copied.
        // Candidates are tried in the order they were added, so the renderer adds its preferred one first.
        result<rhi::root_signature_handle> add(std::string name, const rhi::root_signature_description& description);

        // One stage of a pipeline: its stage (for visibility) and what its shader binds
        struct stage_bindings
        {
            shader_stage stage;
            const shader_bindings* bindings;
        };

        // The first candidate compatible with every stage (§10.1). no_compatible_root_signature lists each
        // candidate with its first mismatch ("standard: pixel shader b3 space0 (64 bytes) not visible").
        result<rhi::root_signature_handle> find_compatible(std::span<const stage_bindings> stages) const;

        // root_signature_incompatible names the first mismatch
        result<void> check_compatible(rhi::root_signature_handle signature,
                                      std::span<const stage_bindings> stages) const;

    private:
        struct candidate;                   // name, handle, owned copy of the description
        rhi::pipelines pipelines_;
        mutable std::shared_mutex mutex_;   // add is rare (startup, a new binding model); lookups are shared
        std::vector<candidate> candidates_;
    };
}
```

### 10.1 Compatibility

A signature is compatible with a pipeline when, for each stage's `shader_bindings`, every binding is provided by a parameter or static sampler **visible to that stage**: visibility `all`, or the stage's own (`vertex` for the vertex shader, `pixel` for the pixel shader); a compute shader only sees `all`.

| Shader uses | Signature must have, at the same register and space, visible to the stage |
|---|---|
| constant buffer of `size` bytes | root `constants` with `constant_count × 4 ≥ size`, or a root `constant_buffer` |
| buffer by register (`t`/`u`) | root `shader_resource` / `unordered_access` of that kind |
| sampler by register | a static sampler |
| `ResourceDescriptorHeap` / `SamplerDescriptorHeap` | nothing: every signature allows it (09 §12.1) |

Parameters the shaders don't use are fine. A pipeline's stages are checked together against the same signature, each with its own visibility, so a vertex shader's `b0` and a pixel shader's `b0` can be matched by two different stage-limited parameters.

### 10.2 Use by the pipeline cache

[§11](#11-graphicspipeline_cacheh): the graphics descriptions get an optional `root_signature`. On a cache miss, the cache resolves the shaders, then either checks the named signature or calls `find_compatible`, and puts the resulting handle into the RHI description. The handle is part of the cache key, so the matching runs once per distinct pipeline, never per draw.

### 10.3 Consequences for the renderer

- The renderer creates a `root_signature_registry` after the RHI, adds `binding_layout::standard_description()` first, then any other model it uses, and passes the registry to its `pipeline_cache`.
- The **redundant-state filter** ([11 §4.8](11-renderer.md#48-recording-instanced-draws-and-the-redundant-state-filter)) must forget its remembered root arguments when the root signature changes, not only when the pipeline changes. The filter can compare the pipelines' signatures, which the pipeline variant registry records next to each handle.
- Passes that use the standard signature only, which is every pass today, see no difference apart from the helper names.

## 11. `graphics/pipeline_cache.h`

```cpp
namespace avis::graphics
{
    // The same as the RHI descriptions, with shader ids instead of bytecode
    struct graphics_pipeline_description
    {
        shader_id vertex_shader;
        shader_id pixel_shader;                    // {} = depth-only
        rhi::graphics_pipeline_state state;        // rasterizer, depth, blend, formats, topology (09 §12)
        std::optional<rhi::root_signature_handle> root_signature;   // checked when set, else matched (§10)
        std::string_view debug_name;
    };

    struct compute_pipeline_description
    {
        shader_id compute_shader;
        std::optional<rhi::root_signature_handle> root_signature;
        std::string_view debug_name;
    };

    class pipeline_cache
    {
    public:
        // Takes only the RHI's pipelines view (09 §4.3): creating PSOs is all it does with the RHI
        pipeline_cache(rhi::pipelines pipelines, const root_signature_registry& root_signatures);
        // Pipelines live as long as the RHI (D-87); the cache only forgets the mapping. If the RHI
        // gains pipeline destroy, the cache must drop entries it destroys (D-87 reminder, step 4).

        // Thread-safe: the renderer's startup bulk calls it from every worker
        result<rhi::graphics_pipeline_handle> get_or_create(const shader_library& shaders,
                                                            const graphics_pipeline_description& description);
        result<rhi::compute_pipeline_handle> get_or_create(const shader_library& shaders,
                                                           const compute_pipeline_description& description);

    private:
        // key = hash over the state fields (not debug_name), the library's serial, the shader ids and the explicit
        // root signature if any; libraries are immutable, so (serial, id) identifies the bytecode without hashing it
        rhi::pipelines pipelines_;
        const root_signature_registry& root_signatures_;
        std::shared_mutex mutex_;                  // shared for lookups, exclusive for inserts (Q1)
        std::unordered_map<std::uint64_t, rhi::graphics_pipeline_handle> graphics_;
        std::unordered_map<std::uint64_t, rhi::compute_pipeline_handle> compute_;
    };
}
```

- **Resolve and check:** each id is looked up in the library (`shader_not_found` naming the id and the pipeline's debug name) and its stage checked against its slot (`shader_stage_mismatch`), before any RHI call.
- **Root signature** ([§10](#10-graphicsroot_signature_registryh)): a named signature is checked with `check_compatible`; otherwise `find_compatible` picks the first registered candidate that fits all stages' bindings, each checked with its stage's visibility (the stage comes from the library entry). A shader without bindings needs a named signature (`shader_bindings_missing`). The result goes into the RHI description's `root_signature`; it's computed once per cache key.
- **Creation outside the lock.** A miss builds the `rhi::graphics_pipeline_description` with the library's bytecode spans and calls `rhi::pipelines::create_pipeline` without holding the mutex, then inserts under the exclusive lock. If another worker inserted the same key meanwhile, the second PSO is kept alive by the RHI (no `destroy`, D-87) but unused, and the first handle returned, so the startup `bulk` never serialises on PSO compilation. A duplicate PSO only happens when two workers race on the same description, which the renderer's variant registry avoids in practice.
- The renderer's `pipeline_variant_registry` ([11 §4](11-renderer.md)) keys its variants and calls the cache; nothing else in the renderer creates pipelines.
- The library is only read during `get_or_create`, so it can be released once the startup pipelines exist; later variants need it again.

## 12. Testing

### 12.1 Shader library and pipeline cache

- `shader_library::create`: duplicate ids, hash collisions (with a crafted pair), empty bytecode; `find` by literal id.
- `load_shader_library`: hash mismatch names the entry; a missing blob gives the IO error with the path; entries without hashes load.
- `pipeline_cache` on WARP: equal descriptions return the same handle; different state or a different library creates a new one; unknown id and wrong stage fail before any RHI call; concurrent `get_or_create` from a `bulk` returns one handle per description; root signature matching as in §12.2.

### 12.2 Root signatures and matching

The RHI's root signature tests are in [09 §22.5](09-rhi.md#225-testing).

- `root_signature_registry` (no GPU needed for the matching itself, with a fake RHI or WARP): first compatible in order; a constant buffer too large for root constants but fine for a root CBV; a missing sampler register; a pixel shader's binding provided only by a vertex-only parameter fails; a compute shader's binding provided only by a stage-limited parameter fails; a mismatch message naming stage, register, space and candidate; `check_compatible` on an explicit signature.
- `pipeline_cache`: matching runs once per key; a shader without bindings and no explicit signature fails with `shader_bindings_missing`.
- The manifest round-trip test (12a §6) includes bindings, so the build tool and the parser can't drift apart.

## 13. Changes to other documents

Applied with this draft:
- **09:** the RHI design of root signatures (09 §12.1, D-74): `root_signature_handle`, `create_root_signature`, the root fields of the pipeline descriptions and the generic root calls (09 §7), plus the header map (09 §2.1), the example (09 §17) and the decisions (09 §18).
- **09, Part II:** the private side (§8); RHI tests in 09 §22.5.
- **Shader library and pipeline cache (§3, §11):** `bindings` on the manifest entry, blob and library entry; optional `root_signature` in the graphics descriptions; the cache takes a registry.
- **17 §2:** the three new error kinds.
- **13 §1, §3, §4, §5:** `-Fre`, the shader list, and `dt shader-manifest` writing the manifest (replacing the `cmake -P` writer and its host hashing tool); Python with `uv` is now needed by the build.
- **11 §13:** `bindless.hlsli` mirrors `graphics::binding_layout`.
- **10 §3.2, 11 §4.8:** `push_constants` and `set_pass_constants` in examples become the `binding_layout` helpers, and the redundant-state filter resets on a root signature change (§10.3).
- **99:** D-106 for root signatures as handles. (D-34 says the graphics layer publishes the binding layout; Q3 and Q4 are D-82 and D-83.)

Left to apply:
- **12a:** manifest format 2 with `bindings` (Q3 decided A): the version check accepts 2, and the parser reads `bindings` into `shader_manifest_entry::bindings`.

## 14. Open questions

### Q1. Pipeline cache reads — decided: A
**A `std::shared_mutex`** (D-104, §11): lookups take it shared, inserts exclusive, and PSO creation runs outside it. The cache is on the creation path only: the startup `bulk`, where nearly every call is a miss that spends milliseconds compiling, and new variants later. Per-frame drawing reads PSO handles from the renderer's `pipeline_variant_registry` ([11 §4](11-renderer.md)), not from the cache. An uncontended shared lock is one atomic operation, the lock is never held across a compile, so it's brief blocking as 15 §4.3 allows, and it matches `root_signature_registry` (§10).

(Moved from the RHI design, which once called the cache's lookups lock-free; 09 no longer does.)
- **A (chosen for iteration 1): `std::shared_mutex`** (§11). Lookups happen at startup and when a new variant appears, so a shared lock costs nothing measurable, and creation already happens outside the lock.
- **B: a lock-free hash map** (or an immutable map published by atomic pointer swap on insert). It only speeds up hits, which the renderer barely makes; it's code without a user. The copy-on-insert variant copies the map on every startup insert, and `std::atomic<std::shared_ptr>` isn't lock-free on MSVC anyway. Neither variant avoids Q2's duplicates, which come from compiling outside the lock.

### Q2. Avoid duplicate PSOs on a race — decided: A
**Accept the rare duplicate** (D-105, §11). When two workers miss on the same key at once, both compile; the first insert wins, both callers get its handle, and the second PSO stays alive unused, because the RHI has no pipeline `destroy` (D-87). It only happens when two workers race on the same description, which the renderer's variant registry avoids in practice, and no worker ever waits for another worker's compile. If the RHI later gains pipeline `destroy`, the cache can destroy the loser ([09 D-87](09-rhi.md#d-87-pipeline-lifetime), option B).

- **A (chosen): accept the rare duplicate** (§11). It costs one unused PSO for the RHI's lifetime and keeps lookups and creation simple.
- **B: an "in progress" entry per key**, so a second worker waits for the first worker's result instead of compiling. No duplicates, but a worker then blocks on another worker, which the parallel scheduler's rules discourage ([15](15-parallel-scheduler.md)).

### Q3. Source of the reflection data — decided: A
**Build time into the manifest** (D-82, §6), written by `dt shader-manifest` in `tools/data-trace`. Runtime reflection (B) stays possible later for tools that compile shaders at runtime, since it produces the same `shader_bindings`.

- **A (chosen): build time into the manifest.** No runtime dependency, errors at build time, and the manifest stays the one description of each shader.
- **B: runtime reflection** with `dxcompiler.dll`. Best for tools that compile at runtime; can be added next to A later.
- **C: hand-written declarations.** No tooling, but drift is only caught by tests.
- **D: embedded root signatures.** Moves the choice of layout to the shader author, which is what this design avoids.
- **E: no reflection,** explicit signatures only.

### Q4. Choosing among compatible candidates — decided: A
**First compatible in registration order** (D-83, §10). A pipeline can still name its signature explicitly, which turns matching into a check (§10.2).

- **A (chosen): first compatible in registration order.** Predictable, and the renderer controls the preference by the order of `add`.
- **B: the smallest compatible signature** (fewest DWORDs). Slightly cheaper root arguments, but which signature a pipeline gets can change when a candidate is added, and with it where the renderer must set its arguments.
- **C: explicit only, matching as a check.** Every pipeline names its signature; the registry only verifies. Safest, but the renderer has to know each pipeline's signature up front.

The former question on shader visibility per root parameter moved with the RHI design to [D-74](09-rhi.md#d-74-shader-visibility-per-root-parameter).
