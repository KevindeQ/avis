#include "ui/viewport_renderer.h"

#include "rendering/shared_frame_ring.h"
#include "ui/viewport_item.h"

#include "avis/log.h"

#include <QColor>
#include <QFile>
#include <QObject>
#include <QQuickRhiItem>
#include <QQuickWindow>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <rhi/qrhi.h>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <Windows.h>
#include <wrl/client.h>

namespace avis::visualizer::ui
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        namespace rendering = avis::visualizer::rendering;

        constexpr std::uint32_t slot_count = rendering::ring_slot_count;

        // How long the renderer's destruction waits for Qt's queue to finish with a ring
        constexpr DWORD release_timeout_ms = 5'000;

        std::string hresult_text(const HRESULT result)
        {
            return std::format("HRESULT 0x{:08X}", static_cast<std::uint32_t>(result));
        }

        QShader load_shader(const QString& name)
        {
            QFile file{ QStringLiteral(":/avis/visualizer/shaders/%1.qsb").arg(name) };
            return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
        }

        // A ring as Qt opened it, from qt_attach until Qt's queue is done with it after qt_detach
        struct imported_ring
        {
            std::shared_ptr<rendering::shared_ring_state> state;
            ComPtr<ID3D12Fence> produced;
            ComPtr<ID3D12Fence> consumed;
            std::array<ComPtr<ID3D12Resource>, slot_count> resources;
            std::array<std::unique_ptr<QRhiTexture>, slot_count> textures; // createFrom: they don't own the resources
            std::array<std::unique_ptr<QRhiShaderResourceBindings>, slot_count> bindings;
            std::unique_ptr<QRhiGraphicsPipeline> pipeline;
            std::uint64_t final_use = 0; // qt_detach's use: released once `consumed` reaches it
        };

        // A Signal(consumed, use) issued after the frame's submission
        struct pending_signal
        {
            ComPtr<ID3D12Fence> fence;
            std::uint64_t value = 0;
        };

        class viewport_renderer final : public QQuickRhiItemRenderer
        {
        public:
            viewport_renderer() = default;
            ~viewport_renderer() override;

            viewport_renderer(const viewport_renderer&) = delete;
            viewport_renderer& operator=(const viewport_renderer&) = delete;

        protected:
            void initialize(QRhiCommandBuffer* command_buffer) override;
            void synchronize(QQuickRhiItem* item) override;
            void render(QRhiCommandBuffer* command_buffer) override;

        private:
            void follow_current_ring();
            std::unique_ptr<imported_ring> import(const std::shared_ptr<rendering::shared_ring_state>& state);
            bool create_pipeline(imported_ring& ring);
            void detach(std::unique_ptr<imported_ring> ring);
            void release_finished();
            void hand_back(QRhiCommandBuffer& command_buffer, imported_ring& ring, std::uint32_t slot);
            void signal_pending();

            rendering::shared_frame_rings* rings_ = nullptr; // the item's, copied in synchronize()
            QMetaObject::Connection frame_end_;

            // Qt's, not owned; null until initialize() found the D3D12 backend
            ID3D12Device* device_ = nullptr;
            ID3D12CommandQueue* queue_ = nullptr;
            bool usable_ = false; // D3D12, with the blit shaders

            QShader vertex_shader_;
            QShader fragment_shader_;
            std::unique_ptr<QRhiSampler> sampler_;
            // Compatible with the current render target's; the pipelines are created against it
            std::unique_ptr<QRhiRenderPassDescriptor> pass_descriptor_;

            std::unique_ptr<imported_ring> shown_; // the ring Qt draws from
            std::unique_ptr<imported_ring> next_; // a newer ring, attached, until it has a frame
            std::vector<std::unique_ptr<imported_ring>> detached_; // until `consumed` reaches their final use
            std::shared_ptr<rendering::shared_ring_state> refused_; // couldn't be imported; not retried

            std::vector<pending_signal> pending_; // issued in afterFrameEnd
        };

        viewport_renderer::~viewport_renderer()
        {
            QObject::disconnect(frame_end_);
            if (queue_ == nullptr)
            {
                return;
            }

            // The scene graph destroys the renderer between frames, so everything recorded is submitted: signals
            // issued now follow it on Qt's queue
            signal_pending();
            if (next_)
            {
                detach(std::move(next_));
            }
            if (shown_)
            {
                detach(std::move(shown_));
            }
            signal_pending();

            // The opened textures may still be sampled: wait for each ring's last use before releasing them
            const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            for (const std::unique_ptr<imported_ring>& ring : detached_)
            {
                if (event == nullptr || ring->consumed->GetCompletedValue() >= ring->final_use)
                {
                    continue;
                }
                if (FAILED(ring->consumed->SetEventOnCompletion(ring->final_use, event)) ||
                    WaitForSingleObject(event, release_timeout_ms) != WAIT_OBJECT_0)
                {
                    log::get("ui")->warn(
                        "the viewport stopped waiting for Qt's queue to finish with shared frame ring {}",
                        ring->state->generation);
                }
            }
            if (event != nullptr)
            {
                CloseHandle(event);
            }
        }

        void viewport_renderer::initialize(QRhiCommandBuffer* /*command_buffer*/)
        {
            if (device_ == nullptr && !usable_)
            {
                if (rhi()->backend() != QRhi::D3D12)
                {
                    log::get("ui")->error(
                        "the viewport needs Qt Quick on Direct3D 12, but it renders with {}", rhi()->backendName());
                    return;
                }
                const auto* handles = static_cast<const QRhiD3D12NativeHandles*>(rhi()->nativeHandles());
                device_ = static_cast<ID3D12Device*>(handles->dev);
                queue_ = static_cast<ID3D12CommandQueue*>(handles->commandQueue);

                vertex_shader_ = load_shader(QStringLiteral("fullscreen_triangle.vert"));
                fragment_shader_ = load_shader(QStringLiteral("fullscreen_texture.frag"));
                if (!vertex_shader_.isValid() || !fragment_shader_.isValid())
                {
                    log::get("ui")->error("the viewport's blit shaders are missing from the resources");
                    return;
                }
                sampler_.reset(
                    rhi()->newSampler(
                        QRhiSampler::Linear,
                        QRhiSampler::Linear,
                        QRhiSampler::None,
                        QRhiSampler::ClampToEdge,
                        QRhiSampler::ClampToEdge));
                sampler_->create();
                usable_ = true;
            }

            // Called again when the render target changes (a resize): the pipelines must match its pass descriptor
            QRhiRenderPassDescriptor* const current = renderTarget()->renderPassDescriptor();
            if (usable_ && (!pass_descriptor_ || !pass_descriptor_->isCompatible(current)))
            {
                pass_descriptor_.reset(current->newCompatibleRenderPassDescriptor());
                for (imported_ring* ring : { shown_.get(), next_.get() })
                {
                    if (ring != nullptr)
                    {
                        create_pipeline(*ring);
                    }
                }
            }
        }

        void viewport_renderer::synchronize(QQuickRhiItem* item)
        {
            // The GUI thread is blocked here
            rings_ = static_cast<viewport_item*>(item)->frame_rings();
            if (!frame_end_ && item->window() != nullptr)
            {
                // Emitted on the render thread after QRhi::endFrame submitted the frame (12 §3.3 rule 4, task 12.2)
                frame_end_ = QObject::connect(
                    item->window(),
                    &QQuickWindow::afterFrameEnd,
                    item->window(),
                    [this]() { signal_pending(); },
                    Qt::DirectConnection);
            }
        }

        void viewport_renderer::render(QRhiCommandBuffer* command_buffer)
        {
            std::optional<rendering::qt_pick_result> pick;
            if (usable_)
            {
                release_finished();
                follow_current_ring();
                if (shown_ && shown_->pipeline)
                {
                    pick = shown_->state->protocol.qt_pick();
                }
            }

            // Rule 3: Avis published the frame only once `produced` reached it, so this wait costs nothing; it's kept
            // for correctness. Queued before Qt submits the frame's command list in endFrame.
            if (pick)
            {
                queue_->Wait(shown_->produced.Get(), pick->frame);
            }

            command_buffer->beginPass(renderTarget(), QColor::fromRgba(viewport_clear_colour), { 1.0f, 0 });
            if (pick)
            {
                const QSize size = renderTarget()->pixelSize();
                command_buffer->setGraphicsPipeline(shown_->pipeline.get());
                command_buffer->setViewport(
                    QRhiViewport{ 0, 0, static_cast<float>(size.width()), static_cast<float>(size.height()) });
                command_buffer->setShaderResources(shown_->bindings[pick->slot].get());
                command_buffer->draw(3);
            }
            command_buffer->endPass();

            if (pick)
            {
                hand_back(*command_buffer, *shown_, pick->slot);
                pending_.push_back({ .fence = shown_->consumed, .value = pick->use });
            }
        }

        void viewport_renderer::follow_current_ring()
        {
            // qt_attach fails only when the ring was retired first, and then a newer one is already current; a few
            // tries cover resizes racing with this
            for (int attempt = 0; rings_ != nullptr && attempt < 4; ++attempt)
            {
                const std::shared_ptr<rendering::shared_ring_state> current = rings_->qt_current();
                const imported_ring* const newest = next_ ? next_.get() : shown_.get();
                if (!current || current == refused_ || (newest != nullptr && newest->state == current))
                {
                    break;
                }
                if (!current->protocol.qt_attach())
                {
                    continue;
                }
                std::unique_ptr<imported_ring> imported = import(current);
                if (!imported)
                {
                    refused_ = current;
                    break;
                }
                if (next_)
                {
                    detach(std::move(next_)); // replaced before it showed anything
                }
                next_ = std::move(imported);
                break;
            }

            // The previous ring stays on screen until the new one has a frame, so a resize never flashes the clear
            // colour
            if (next_ && (!shown_ || next_->state->protocol.latest_completed()))
            {
                if (shown_)
                {
                    detach(std::move(shown_));
                }
                shown_ = std::move(next_);
            }
        }

        std::unique_ptr<imported_ring>
            viewport_renderer::import(const std::shared_ptr<rendering::shared_ring_state>& state)
        {
            auto ring = std::make_unique<imported_ring>();
            ring->state = state;

            const auto fail = [&](const std::string& reason) -> std::unique_ptr<imported_ring>
            {
                log::get("ui")->error(
                    "the viewport could not open shared frame ring {}: {}", state->generation, reason);
                // Qt attached, so it detaches for Avis to release the ring. Nothing sampled it: the use is signalled
                // at once.
                const std::uint64_t use = state->protocol.qt_detach();
                if (ring->consumed)
                {
                    queue_->Signal(ring->consumed.Get(), use);
                }
                else
                {
                    log::get("ui")->error(
                        "shared frame ring {} stays allocated: its `consumed` fence didn't open", state->generation);
                }
                return nullptr;
            };

            // `consumed` first, so a later failure can still release the ring
            if (const HRESULT result = device_->OpenSharedHandle(
                    static_cast<HANDLE>(state->consumed.value), IID_PPV_ARGS(&ring->consumed));
                FAILED(result))
            {
                return fail("OpenSharedHandle on `consumed`: " + hresult_text(result));
            }
            if (const HRESULT result = device_->OpenSharedHandle(
                    static_cast<HANDLE>(state->produced.value), IID_PPV_ARGS(&ring->produced));
                FAILED(result))
            {
                return fail("OpenSharedHandle on `produced`: " + hresult_text(result));
            }

            const QSize size{ static_cast<int>(state->size.width), static_cast<int>(state->size.height) };
            for (std::uint32_t slot = 0; slot < slot_count; ++slot)
            {
                if (const HRESULT result = device_->OpenSharedHandle(
                        static_cast<HANDLE>(state->textures[slot].value), IID_PPV_ARGS(&ring->resources[slot]));
                    FAILED(result))
                {
                    return fail(std::format("OpenSharedHandle on slot {}: {}", slot, hresult_text(result)));
                }
                ring->textures[slot].reset(rhi()->newTexture(QRhiTexture::RGBA8, size));
                if (!ring->textures[slot]->createFrom(
                        { static_cast<quint64>(reinterpret_cast<std::uintptr_t>(ring->resources[slot].Get())),
                          D3D12_RESOURCE_STATE_COMMON }))
                {
                    return fail(std::format("QRhiTexture::createFrom refused slot {}", slot));
                }
                ring->bindings[slot].reset(rhi()->newShaderResourceBindings());
                ring->bindings[slot]->setBindings(
                    { QRhiShaderResourceBinding::sampledTexture(
                        0, QRhiShaderResourceBinding::FragmentStage, ring->textures[slot].get(), sampler_.get()) });
                if (!ring->bindings[slot]->create())
                {
                    return fail(std::format("the shader resource bindings of slot {} could not be created", slot));
                }
            }
            if (!create_pipeline(*ring))
            {
                return fail("the blit pipeline could not be created");
            }

            log::get("ui")->debug(
                "the viewport opened shared frame ring {} ({}x{})",
                state->generation,
                state->size.width,
                state->size.height);
            return ring;
        }

        bool viewport_renderer::create_pipeline(imported_ring& ring)
        {
            // The three slots' bindings share a layout, so one pipeline serves them all
            std::unique_ptr<QRhiGraphicsPipeline> pipeline{ rhi()->newGraphicsPipeline() };
            pipeline->setShaderStages(
                { { QRhiShaderStage::Vertex, vertex_shader_ }, { QRhiShaderStage::Fragment, fragment_shader_ } });
            pipeline->setCullMode(QRhiGraphicsPipeline::None);
            pipeline->setShaderResourceBindings(ring.bindings[0].get());
            pipeline->setRenderPassDescriptor(pass_descriptor_.get());
            if (!pipeline->create())
            {
                ring.pipeline.reset();
                return false;
            }
            ring.pipeline = std::move(pipeline);
            return true;
        }

        void viewport_renderer::detach(std::unique_ptr<imported_ring> ring)
        {
            // One more use, signalled after this frame like a pick's: Avis releases the ring once `consumed` reaches it
            ring->final_use = ring->state->protocol.qt_detach();
            pending_.push_back({ .fence = ring->consumed, .value = ring->final_use });
            log::get("ui")->debug("the viewport left shared frame ring {}", ring->state->generation);
            detached_.push_back(std::move(ring));
        }

        void viewport_renderer::release_finished()
        {
            std::erase_if(
                detached_,
                [](const std::unique_ptr<imported_ring>& ring)
                { return ring->consumed->GetCompletedValue() >= ring->final_use; });
        }

        void viewport_renderer::hand_back(
            QRhiCommandBuffer& command_buffer, imported_ring& ring, const std::uint32_t slot)
        {
            // Qt tracks the texture in a sampling state after the pass, but Avis writes the next frame from COMMON
            // (12 §3.3 rule 4): a native transition on Qt's command list, then Qt is told
            QRhiTexture& texture = *ring.textures[slot];
            const int layout = texture.nativeTexture().layout;
            if (layout == D3D12_RESOURCE_STATE_COMMON)
            {
                return;
            }
            command_buffer.beginExternal();
            auto* const list = static_cast<ID3D12GraphicsCommandList*>(
                static_cast<const QRhiD3D12CommandBufferNativeHandles*>(command_buffer.nativeHandles())->commandList);
            const D3D12_RESOURCE_BARRIER barrier{ .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                                  .Transition = {
                                                      .pResource = ring.resources[slot].Get(),
                                                      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                      .StateBefore = static_cast<D3D12_RESOURCE_STATES>(layout),
                                                      .StateAfter = D3D12_RESOURCE_STATE_COMMON } };
            list->ResourceBarrier(1, &barrier);
            command_buffer.endExternal();
            texture.setNativeLayout(D3D12_RESOURCE_STATE_COMMON);
        }

        void viewport_renderer::signal_pending()
        {
            for (const pending_signal& signal : pending_)
            {
                queue_->Signal(signal.fence.Get(), signal.value);
            }
            pending_.clear();
        }
    } // namespace

    std::optional<QString> enable_d3d12_debugging(const bool debug_layer, const bool gpu_based_validation)
    {
        if (!debug_layer && !gpu_based_validation)
        {
            return std::nullopt;
        }
        ComPtr<ID3D12Debug1> debug;
        if (const HRESULT result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug)); FAILED(result))
        {
            return QString::fromStdString("D3D12GetDebugInterface failed (" + hresult_text(result) + ")");
        }
        // The same calls, in the same order, as the RHI's debug_state::enable_before_device
        debug->EnableDebugLayer();
        debug->SetEnableGPUBasedValidation(gpu_based_validation ? TRUE : FALSE);
        return std::nullopt;
    }

    std::optional<qt_adapter> read_qt_adapter(QRhi& rhi)
    {
        if (rhi.backend() != QRhi::D3D12)
        {
            return std::nullopt;
        }
        const auto* handles = static_cast<const QRhiD3D12NativeHandles*>(rhi.nativeHandles());
        if (handles == nullptr)
        {
            return std::nullopt;
        }
        return qt_adapter{
            .luid = { .low = static_cast<std::uint32_t>(handles->adapterLuidLow),
                      .high = static_cast<std::int32_t>(handles->adapterLuidHigh) },
            .name = QString::fromUtf8(rhi.driverInfo().deviceName),
        };
    }

    QQuickRhiItemRenderer* make_viewport_renderer()
    {
        return new viewport_renderer;
    }
} // namespace avis::visualizer::ui
