#include "rendering/render_system.h"

#include "rendering/renderer_shaders.h"
#include "rendering/scene_renderer.h"

#include "avis/log.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/transient.h"
#include "avis/threading/parallel_helpers.h"

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        // Prefixes the step that failed to the error's message, keeping its domain and code (12 §7)
        avis::error startup_failure(avis::error error, const std::string_view step)
        {
            error.message = std::format("{}: {}", step, error.message);
            return error;
        }

        template<class T>
        result<T> with_step(result<T> created, const std::string_view step)
        {
            if (!created)
            {
                return std::unexpected(startup_failure(std::move(created.error()), step));
            }
            return created;
        }
    } // namespace

    render_system::render_system() noexcept = default;

    result<std::unique_ptr<render_system>> render_system::create(const render_system_configuration& configuration)
    {
        const render_settings& settings = configuration.settings;
        std::unique_ptr<render_system> system{ new render_system };

        rhi::render_hardware_interface_configuration device;
        device.adapter.adapter = configuration.adapter;
        device.adapter.allow_software_adapter = configuration.allow_software_adapter;
        device.debug.debug_layer = settings.debug_layer || settings.gpu_validation;
        device.debug.gpu_based_validation = settings.gpu_validation;
        auto created =
            with_step(rhi::render_hardware_interface::create(device), "the graphics device could not be created");
        if (!created)
        {
            return std::unexpected(std::move(created.error()));
        }
        system->rhi_ = std::move(*created);
        rhi::render_hardware_interface& hardware = *system->rhi_;
        const rhi::hardware_capabilities& capabilities = hardware.capabilities();
        log::get("render")->info(
            "the renderer's device is on {} (LUID {:08x}:{:08x}){}{}",
            capabilities.adapter_name,
            static_cast<std::uint32_t>(capabilities.luid.high),
            capabilities.luid.low,
            device.debug.debug_layer ? ", with the debug layer" : "",
            device.debug.gpu_based_validation ? " and GPU-based validation" : "");

        const rhi::queue<rhi::queue_type::graphics> graphics_queue{ hardware };
        const rhi::queue<rhi::queue_type::compute> compute_queue{ hardware };
        const rhi::queue<rhi::queue_type::copy> copy_queue{ hardware };

        auto frames = with_step(
            graphics::frame_scheduler::create(
                rhi::synchronization{ hardware },
                rhi::transient_objects{ hardware },
                { .frames_in_flight = settings.frames_in_flight,
                  .upload_memory_per_frame = configuration.upload_memory_per_frame }),
            "the frame scheduler could not be created");
        if (!frames)
        {
            return std::unexpected(std::move(frames.error()));
        }
        system->frames_ = std::move(*frames);
        const std::uint32_t frames_in_flight = system->frames_->frames_in_flight();

        auto profiler = with_step(
            graphics::gpu_profiler::create(
                rhi::transient_objects{ hardware }, graphics_queue, compute_queue, copy_queue, frames_in_flight, {}),
            "the GPU profiler could not be created");
        if (!profiler)
        {
            return std::unexpected(std::move(profiler.error()));
        }
        system->profiler_ = std::move(*profiler);

        system->transient_pool_ = std::make_unique<graphics::transient_resource_pool>(
            rhi::memory{ hardware }, rhi::resources{ hardware }, frames_in_flight);
        system->graph_ = std::make_unique<graphics::render_graph>(
            rhi::resources{ hardware },
            rhi::command_lists{ hardware },
            graphics_queue,
            compute_queue,
            copy_queue,
            *system->transient_pool_,
            *system->frames_,
            system->profiler_.get(),
            graphics::render_graph_configuration{
                .async_compute = settings.async_compute,
                .aliasing = settings.graph_aliasing,
            });

        auto uploads = with_step(
            graphics::upload_queue::create(
                rhi::resources{ hardware },
                rhi::command_lists{ hardware },
                copy_queue,
                rhi::synchronization{ hardware },
                {}),
            "the upload queue could not be created");
        if (!uploads)
        {
            return std::unexpected(std::move(uploads.error()));
        }
        system->uploads_ = std::move(*uploads);

        auto readbacks = with_step(
            graphics::readback_queue::create(
                rhi::resources{ hardware },
                rhi::command_lists{ hardware },
                graphics_queue,
                compute_queue,
                copy_queue,
                rhi::synchronization{ hardware }),
            "the readback queue could not be created");
        if (!readbacks)
        {
            return std::unexpected(std::move(readbacks.error()));
        }
        system->readbacks_ = std::move(*readbacks);

        system->root_signatures_ = std::make_unique<graphics::root_signature_registry>(rhi::pipelines{ hardware });
        system->pipeline_cache_ =
            std::make_unique<graphics::pipeline_cache>(rhi::pipelines{ hardware }, *system->root_signatures_);

        // On the calling thread, where waiting is allowed at startup (12 §10.3)
        auto library = with_step(
            threading::sync_wait_result(load_renderer_shaders(configuration.shader_directory)),
            "the renderer's shaders could not be loaded");
        if (!library)
        {
            return std::unexpected(std::move(library.error()));
        }
        system->shaders_ = std::make_unique<graphics::shader_library>(std::move(*library));
        log::get("render")->info("loaded the renderer's shaders from {}", configuration.shader_directory.string());

        rhi::render_hardware_interface* const budget_source = system->rhi_.get();
        auto renderer = with_step(
            scene_renderer::create(
                renderer_dependencies{
                    .resources = rhi::resources{ hardware },
                    .pipelines = rhi::pipelines{ hardware },
                    .synchronization = rhi::synchronization{ hardware },
                    .graph = *system->graph_,
                    .pool = *system->transient_pool_,
                    .frames = *system->frames_,
                    .profiler = system->profiler_.get(),
                    .pipeline_cache = *system->pipeline_cache_,
                    .root_signatures = *system->root_signatures_,
                    .uploads = *system->uploads_,
                    .memory_budget = [budget_source] { return budget_source->local_memory_budget(); },
                },
                settings,
                *system->shaders_),
            "the renderer could not be created");
        if (!renderer)
        {
            return std::unexpected(std::move(renderer.error()));
        }
        system->renderer_ = std::move(*renderer);
        return system;
    }

    render_system::~render_system()
    {
        // The upload and readback queues must have nothing outstanding, and the renderer's last uploads may still
        // run on the copy queue: wait for the GPU, which also completes what waits on it, then go in reverse
        if (rhi_ != nullptr)
        {
            rhi_->wait_idle();
        }
        renderer_.reset();
        shaders_.reset();
        pipeline_cache_.reset();
        root_signatures_.reset();
        readbacks_.reset();
        uploads_.reset();
        graph_.reset();
        transient_pool_.reset();
        profiler_.reset();
        frames_.reset();
        rhi_.reset(); // its destructor waits for the GPU again and frees the deferred releases
    }

    const rhi::hardware_capabilities& render_system::capabilities() const noexcept
    {
        return rhi_->capabilities();
    }

    rhi::completion_driver render_system::completion_driver() noexcept
    {
        return rhi::completion_driver{ *rhi_ };
    }

    rhi::synchronization render_system::synchronization() noexcept
    {
        return rhi::synchronization{ *rhi_ };
    }

    graphics::frame_scheduler& render_system::frames() noexcept
    {
        return *frames_;
    }

    graphics::gpu_profiler& render_system::profiler() noexcept
    {
        return *profiler_;
    }

    graphics::render_graph& render_system::graph() noexcept
    {
        return *graph_;
    }

    graphics::upload_queue& render_system::uploads() noexcept
    {
        return *uploads_;
    }

    graphics::readback_queue& render_system::readbacks() noexcept
    {
        return *readbacks_;
    }

    scene_renderer& render_system::renderer() noexcept
    {
        return *renderer_;
    }

    rhi::render_hardware_interface& render_system::hardware_interface() noexcept
    {
        return *rhi_;
    }
} // namespace avis::visualizer::rendering
