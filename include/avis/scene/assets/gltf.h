#ifndef AVIS_SCENE_ASSETS_GLTF_H
#define AVIS_SCENE_ASSETS_GLTF_H

// glTF import (docs/design/08-scene-management.md §5.1, §5.3, §9.4): what a load gives back, and the named sender that
// asset_cache::load_gltf returns (D-66). The operation's state, fastgltf included, is in the private
// lib/avis/scene/assets/gltf_import.h, so this header reaches neither fastgltf nor <windows.h>.

#include "avis/detail/receiver_stop_callback.h"
#include "avis/error.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/types.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace avis::scene
{
    struct gltf_node
    {
        std::string name;
        transform local; // already in Avis axes (Z up)
        std::int32_t parent = -1; // index into gltf_asset_set::nodes
        mesh_handle mesh; // invalid = no mesh
        std::vector<material_handle> materials; // one per primitive
    };

    // Holds one reference to each handle; release with asset_cache::release_all
    struct gltf_asset_set
    {
        std::filesystem::path path;
        std::vector<mesh_handle> meshes;
        std::vector<material_handle> materials;
        std::vector<texture_handle> textures;
        std::vector<gltf_node> nodes; // parents before children
    };

    class asset_cache;

    namespace detail
    {
        struct gltf_load_core;

        // Non-template part of a load_gltf operation
        class gltf_load_operation
        {
        public:
            // Called once: inline from begin() for a cache without IO, otherwise on an IO thread (a failed read) or a
            // parallel worker
            virtual void complete(result<gltf_asset_set> outcome) noexcept = 0;
            virtual void complete_stopped() noexcept = 0;

        protected:
            gltf_load_operation(asset_cache& cache, std::filesystem::path path);
            ~gltf_load_operation();
            gltf_load_operation(const gltf_load_operation&) = delete;
            gltf_load_operation& operator=(const gltf_load_operation&) = delete;

            // Starts the read. The operation may complete (and be destroyed) before begin() returns.
            void begin() noexcept;

            // From the stop callback, on any thread. Honoured between the stages: the read and the external file
            // reads are cancelled, the image decodes still running finish, then the load completes with set_stopped.
            void request_stop() noexcept;

        private:
            std::unique_ptr<gltf_load_core> core_;
        };

        template<class Receiver>
        class gltf_load_operation_state final : public gltf_load_operation
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            gltf_load_operation_state(Receiver receiver, asset_cache& cache, std::filesystem::path path) :
                gltf_load_operation{ cache, std::move(path) },
                receiver_{ std::move(receiver) }
            {}

            void start() & noexcept
            {
                if (stop_callback::stop_requested(receiver_))
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                stop_callback_.install(receiver_, stop_function{ this });
                begin();
            }

            void complete(result<gltf_asset_set> outcome) noexcept override
            {
                stop_callback_.reset();
                if (outcome)
                {
                    stdexec::set_value(std::move(receiver_), std::move(*outcome));
                }
                else
                {
                    stdexec::set_error(std::move(receiver_), std::move(outcome.error()));
                }
            }

            void complete_stopped() noexcept override
            {
                stop_callback_.reset();
                stdexec::set_stopped(std::move(receiver_));
            }

        private:
            struct stop_function
            {
                gltf_load_operation_state* self;

                void operator()() const noexcept
                {
                    self->request_stop();
                }
            };

            using stop_callback = avis::detail::receiver_stop_callback<Receiver, stop_function>;

            Receiver receiver_;
            stop_callback stop_callback_;
        };
    } // namespace detail

    // Completes with set_value(gltf_asset_set), set_error(avis::error) or set_stopped(). Single-shot. The cache must
    // outlive the operation.
    class gltf_load_sender
    {
    public:
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = avis::completions<gltf_asset_set>;

        template<stdexec::receiver Receiver>
        auto connect(Receiver receiver) && -> detail::gltf_load_operation_state<Receiver>
        {
            return { std::move(receiver), *cache_, std::move(path_) };
        }

    private:
        friend class asset_cache;

        gltf_load_sender(asset_cache& cache, std::filesystem::path path) : cache_{ &cache }, path_{ std::move(path) } {}

        asset_cache* cache_;
        std::filesystem::path path_;
    };

    static_assert(avis::sender_of<gltf_load_sender, gltf_asset_set>);
} // namespace avis::scene

#endif
