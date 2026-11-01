#ifndef AVIS_SCENE_ASSETS_GLTF_IMPORT_H
#define AVIS_SCENE_ASSETS_GLTF_IMPORT_H

// The state of one asset_cache::load_gltf (docs/tasks/10-scene-management.md §10.6). Private: it includes fastgltf.
//
// The stages, each one started by the previous one's completion:
//   1. read the file (io::read_file), on an IO thread
//   2. on a parallel worker: hash and look up the deduplication table, parse with fastgltf, then read the external
//      buffers and images (io::read_file each, counted with an atomic)
//   3. on a parallel worker: attach the external bytes, check the buffer ranges, validate, convert the materials,
//      the geometry and the nodes, then decode the images with one bulk item each
//   4. on the worker that ran the last bulk item: create the textures, materials and meshes, and complete
// A stop request is honoured between the stages; a failure in stage 4 releases what it created.

#include "avis/error.h"
#include "avis/io/file_operations.h"
#include "avis/scene/assets/gltf.h"
#include "avis/scene/assets/material.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/assets/texture_data.h"

#include <fastgltf/core.hpp>
#include <stdexec/execution.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace avis::scene::detail
{
    // Test hook: called at the start of every image decode, with the decode's index, from a parallel worker. An empty
    // function removes it. Tests use it to request a stop while decoding.
    void set_image_decode_hook(std::function<void(std::size_t)> hook);

    enum class gltf_stage : std::uint8_t
    {
        parse,
        prepare
    };

    // Receives the file read (resource == main_file) or one external buffer or image read
    struct gltf_read_receiver
    {
        using receiver_concept = stdexec::receiver_tag;
        static constexpr std::size_t main_file = static_cast<std::size_t>(-1);

        void set_value(std::vector<std::byte> bytes) noexcept;
        void set_error(avis::error error) noexcept;
        void set_stopped() noexcept;

        auto get_env() const noexcept
        {
            return stdexec::prop{ stdexec::get_stop_token, token };
        }

        gltf_load_core* core;
        std::size_t resource;
        stdexec::inplace_stop_token token;
    };

    // Receives a hop to the parallel scheduler. No stop token: the hop always runs, and the stage decides.
    struct gltf_hop_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value() noexcept;
        template<class Error>
        void set_error(Error&& error) noexcept;
        void set_stopped() noexcept;

        stdexec::env<> get_env() const noexcept
        {
            return {};
        }

        gltf_load_core* core;
        gltf_stage stage;
    };

    // One image decode
    struct gltf_decode_item
    {
        void operator()(std::size_t index) const noexcept;

        gltf_load_core* core;
    };

    // Receives the end of the image decodes
    struct gltf_decode_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value() noexcept;
        template<class Error>
        void set_error(Error&& error) noexcept;
        void set_stopped() noexcept;

        stdexec::env<> get_env() const noexcept
        {
            return {};
        }

        gltf_load_core* core;
    };

    using gltf_read_operation =
        decltype(stdexec::connect(std::declval<io::read_file_sender>(), std::declval<gltf_read_receiver>()));
    using gltf_hop_operation = decltype(stdexec::connect(
        stdexec::schedule(std::declval<stdexec::parallel_scheduler>()), std::declval<gltf_hop_receiver>()));
    using gltf_decode_sender = decltype(stdexec::bulk(
        stdexec::schedule(std::declval<stdexec::parallel_scheduler>()),
        stdexec::par,
        std::size_t{},
        std::declval<gltf_decode_item>()));
    using gltf_decode_operation =
        decltype(stdexec::connect(std::declval<gltf_decode_sender>(), std::declval<gltf_decode_receiver>()));

    // An external file to read: a buffer's or an image's URI
    struct gltf_external_resource
    {
        bool buffer = true; // else an image
        std::size_t index = 0; // into the asset's buffers or images
        std::size_t offset = 0; // sources::URI::fileByteOffset
        std::filesystem::path path;
    };

    // One image to decode: a glTF image read as colour (sRGB) or data
    struct gltf_image_request
    {
        std::size_t image = 0;
        bool srgb = true;
        std::span<const std::byte> bytes; // into the asset's buffers, a data URI or an external file
        std::string description;
    };

    // A material converted but not yet created: the textures are indices into the image requests
    struct gltf_pending_material
    {
        material_description description;
        std::array<std::optional<std::size_t>, 5> textures; // base colour, metallic-roughness, normal, occlusion,
                                                            // emissive
    };

    struct gltf_pending_mesh
    {
        std::string name;
        mesh_data data;
        std::vector<std::size_t> materials; // per primitive, into the pending materials
    };

    struct gltf_pending_node
    {
        std::string name;
        transform local;
        std::int32_t parent = -1;
        std::optional<std::size_t> mesh;
    };

    struct gltf_load_core
    {
        gltf_load_core(gltf_load_operation& owner, asset_cache& cache, std::filesystem::path path);

        void begin() noexcept;
        void request_stop() noexcept;

        // Stage plumbing (gltf_import.cpp)
        void hop(gltf_stage stage) noexcept;
        void run(gltf_stage stage) noexcept;
        void run_parse() noexcept;
        void run_prepare() noexcept;
        void start_reads() noexcept;
        void release_read() noexcept;
        void start_decodes() noexcept;
        void decode(std::size_t index) noexcept;
        void after_decodes() noexcept;
        void fail(avis::error error) noexcept; // records the first failure of the external reads

        // The stages' work; may throw (caught by the plumbing)
        result<std::optional<gltf_asset_set>> parse();
        result<void> prepare();
        result<void> attach_external_bytes();
        result<void> check_ranges() const;
        result<void> convert_materials();
        result<void> convert_meshes();
        result<void> convert_nodes();
        result<void> create_assets(gltf_asset_set& created);
        result<gltf_asset_set> create_assets_or_release() noexcept;

        // Image request index for a material's texture, created on first use; nullopt (with a warning) when the
        // texture has no decodable image
        result<std::optional<std::size_t>> request_image(std::size_t texture, bool srgb);

        // A warning on the "scene" logger, once per message and load
        void warn(const std::string& message);

        // Completes the load; touches nothing afterwards
        void finish(result<gltf_asset_set> outcome) noexcept;
        void finish_stopped() noexcept;

        gltf_load_operation* owner;
        asset_cache* cache;
        std::filesystem::path path;
        std::filesystem::path canonical_path;
        std::uint64_t hash = 0;

        std::vector<std::byte> file;
        std::optional<fastgltf::Asset> asset;
        std::vector<gltf_external_resource> resources;
        std::vector<std::vector<std::byte>> external; // one per resource, written by its read
        std::vector<std::span<const std::byte>> image_bytes; // per glTF image, once attached

        std::vector<gltf_image_request> images;
        std::vector<std::optional<result<texture_data>>> decoded; // per request; nullopt when skipped by a stop
        std::vector<gltf_pending_material> materials; // the glTF materials, then the default one if needed
        std::optional<std::size_t> default_material;
        std::vector<gltf_pending_mesh> meshes; // per glTF mesh
        std::vector<gltf_pending_node> nodes; // parents before children
        std::set<std::string> warned;

        std::unique_ptr<gltf_read_operation> main_read;
        std::vector<std::unique_ptr<gltf_read_operation>> reads;
        std::array<std::unique_ptr<gltf_hop_operation>, 2> hops; // one per stage: a running hop is never replaced
        std::unique_ptr<gltf_decode_operation> decodes;
        std::atomic<std::size_t> pending{ 0 }; // outstanding external reads, plus one while starting them

        stdexec::inplace_stop_source stop_source; // stops the reads
        std::atomic<bool> stop_requested{ false }; // the receiver asked to stop
        std::atomic<bool> read_stopped{ false }; // an external read completed with set_stopped
        std::mutex error_mutex;
        std::optional<avis::error> first_error; // of the external reads; read after every read released
    };
} // namespace avis::scene::detail

#endif
