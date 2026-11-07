// The interop spike of docs/tasks/12-visualizer.md §12.2, kept as a test so a Qt upgrade re-checks it. On Qt's real
// D3D12 device, with the threaded render loop, it checks the two assumptions the shared frame ring (12 §3.3) rests on:
// - QQuickWindow::afterFrameEnd is emitted on the render thread after QRhi::endFrame has submitted the frame's command
//   list, so a Signal(consumed, m) issued there follows Qt's sampling (rule 4's direct path)
// - QRhiTexture::createFrom accepts a texture opened with OpenSharedHandle in state COMMON, and Qt samples what the
//   Avis RHI wrote into it; setNativeLayout hands the texture back in COMMON after each frame
//
// Native D3D12 is fine here: the rule that only ui/viewport_renderer.cpp uses it (12 §3.2) is about apps/visualizer.

#include "helpers/log_capture.h"
#include "ui_tests.h"

#include "avis/rhi/barriers.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/shared.h"

#include <QFile>
#include <QQuickRhiItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <QThread>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
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

namespace
{
    using Microsoft::WRL::ComPtr;
    using namespace avis;

    constexpr int frames_to_check = 100;
    constexpr QSize window_size{ 256, 256 };
    constexpr QSize shared_size{ 64, 64 };
    constexpr std::array avis_colour{ 0.2f, 0.6f, 0.4f, 1.0f };
    constexpr std::array<int, 4> expected_pixel{ 51, 153, 102, 255 }; // avis_colour × 255
    constexpr DWORD gate_timeout_ms = 10'000;

    std::string message_of(const avis::error& error)
    {
        return error.message;
    }

    // What the GUI thread (the test) and Qt's render thread (the renderer) share. Each field is written by one side;
    // plain fields are published by the atomic flag written after them.
    // Where the renderer checks a frame's submission: afterFrameEnd is the claim under test; afterRendering, which Qt
    // documents as emitted before the frame's commands are submitted, is the control that shows the check can fail
    enum class check_signal : std::uint8_t
    {
        after_frame_end,
        after_rendering
    };

    struct interop_state
    {
        check_signal hook = check_signal::after_frame_end; // set before the window exists

        // Render thread → test
        rhi::adapter_luid qt_adapter;
        std::atomic<bool> adapter_known{ false };
        std::atomic<Qt::HANDLE> render_thread{ nullptr };
        std::atomic<Qt::HANDLE> check_thread{ nullptr }; // where the hook ran
        std::atomic<bool> imported{ false };
        std::atomic<int> layout_after_draw{ -1 }; // Qt's tracked D3D12_RESOURCE_STATES after sampling
        std::atomic<int> layout_handed_back{ -1 }; // after setNativeLayout(COMMON)
        std::atomic<int> checked_frames{ 0 };
        std::atomic<int> submitted_before{ 0 }; // frames whose command list ran before the hook's signal
        std::atomic<std::uint64_t> last_checked{ 0 };
        std::array<int, 4> pixel{};
        std::atomic<bool> pixel_read{ false };
        std::atomic<int> qt_debug_errors{ -1 }; // -1: no info queue (the debug layer isn't on)

        // Test → render thread
        rhi::shared_handle texture;
        rhi::shared_handle produced;
        rhi::shared_handle consumed;
        std::atomic<bool> handles_ready{ false };

        void fail(std::string message)
        {
            const std::scoped_lock lock{ mutex };
            failures.push_back(std::move(message));
        }

        std::vector<std::string> failure_list()
        {
            const std::scoped_lock lock{ mutex };
            return failures;
        }

    private:
        std::mutex mutex;
        std::vector<std::string> failures;
    };

    std::string first_failure(interop_state& state)
    {
        const std::vector<std::string> failures = state.failure_list();
        return failures.empty() ? std::string{} : failures.front();
    }

    QShader load_shader(const QString& name)
    {
        QFile file{ QStringLiteral(":/avis/visualizer/shaders/%1.qsb").arg(name) };
        return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
    }

    // Stands in for ui/viewport_renderer (12.5): opens the shared texture and fences on Qt's device, waits for
    // `produced`, samples the texture, and signals `consumed` from the hook (afterFrameEnd, as 12 §3.3 rule 4 plans).
    // Lives on the render thread.
    class interop_renderer : public QQuickRhiItemRenderer
    {
    public:
        explicit interop_renderer(interop_state& state) : state_(state) {}

        ~interop_renderer() override
        {
            QObject::disconnect(hook_);
            if (event_ != nullptr)
            {
                CloseHandle(event_);
            }
        }

    protected:
        void initialize(QRhiCommandBuffer*) override
        {
            if (device_)
            {
                return;
            }
            if (rhi()->backend() != QRhi::D3D12)
            {
                state_.fail(std::format("Qt's RHI backend is {}, not D3D12", rhi()->backendName()));
                return;
            }

            const auto* handles = static_cast<const QRhiD3D12NativeHandles*>(rhi()->nativeHandles());
            device_ = static_cast<ID3D12Device*>(handles->dev);
            queue_ = static_cast<ID3D12CommandQueue*>(handles->commandQueue);
            state_.render_thread = QThread::currentThreadId();

            // The gate: the hook signals `before_gate_`, then makes the queue wait on `gate_`, which the CPU opens once
            // it has looked at the marker. Anything Qt submits after the hook waits behind the gate.
            if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&before_gate_))) ||
                FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate_))))
            {
                state_.fail("CreateFence failed on Qt's device");
                return;
            }

            // The marker: render() writes the frame number into it from Qt's command list
            const D3D12_HEAP_PROPERTIES readback_heap{ .Type = D3D12_HEAP_TYPE_READBACK };
            const D3D12_RESOURCE_DESC marker_description{ .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                                          .Width = 256,
                                                          .Height = 1,
                                                          .DepthOrArraySize = 1,
                                                          .MipLevels = 1,
                                                          .SampleDesc = { 1, 0 },
                                                          .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
            if (FAILED(device_->CreateCommittedResource(
                    &readback_heap,
                    D3D12_HEAP_FLAG_NONE,
                    &marker_description,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&marker_))))
            {
                state_.fail("CreateCommittedResource failed for the marker");
                return;
            }
            event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

            ComPtr<ID3D12InfoQueue> info_queue;
            if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&info_queue))))
            {
                info_queue_ = info_queue;
            }

            state_.qt_adapter = { handles->adapterLuidLow, handles->adapterLuidHigh };
            state_.adapter_known = true;
        }

        void synchronize(QQuickRhiItem* item) override
        {
            // The GUI thread is blocked here, so connecting is safe. Both signals are emitted on the render thread.
            if (!hook_ && item->window() != nullptr)
            {
                const auto signal = state_.hook == check_signal::after_rendering ? &QQuickWindow::afterRendering :
                                                                                   &QQuickWindow::afterFrameEnd;
                hook_ = QObject::connect(
                    item->window(), signal, item->window(), [this]() { on_hook(); }, Qt::DirectConnection);
            }
        }

        void render(QRhiCommandBuffer* command_buffer) override
        {
            if (!imported_ && state_.handles_ready && device_)
            {
                import_shared();
            }
            if (!imported_)
            {
                command_buffer->beginPass(renderTarget(), Qt::black, { 1.0f, 0 });
                command_buffer->endPass();
                return;
            }

            const std::uint64_t frame = ++frame_;

            // As viewport_renderer will: a GPU wait for Avis's frame, before Qt's command list is submitted
            queue_->Wait(produced_.Get(), 1);

            // The marker write goes into the frame's command list, before the pass
            command_buffer->beginExternal();
            auto* list = static_cast<ID3D12GraphicsCommandList*>(
                static_cast<const QRhiD3D12CommandBufferNativeHandles*>(command_buffer->nativeHandles())->commandList);
            ComPtr<ID3D12GraphicsCommandList2> list2;
            if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list2))))
            {
                state_.fail("Qt's command list has no ID3D12GraphicsCommandList2");
                command_buffer->endExternal();
                return;
            }
            const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker{ marker_->GetGPUVirtualAddress(),
                                                               static_cast<UINT>(frame) };
            const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
            list2->WriteBufferImmediate(1, &marker, &mode);
            command_buffer->endExternal();

            // Qt believes the texture is in COMMON at the start of each frame (setNativeLayout below)
            QRhiResourceUpdateBatch* readback = nullptr;
            if (!readback_requested_)
            {
                readback_requested_ = true;
                readback_.completed = [this]() { on_readback(); };
                readback = rhi()->nextResourceUpdateBatch();
                readback->readBackTexture(QRhiReadbackDescription{ colorTexture() }, &readback_);
            }
            const QSize size = renderTarget()->pixelSize();
            command_buffer->beginPass(renderTarget(), Qt::black, { 1.0f, 0 });
            command_buffer->setGraphicsPipeline(pipeline_.get());
            command_buffer->setViewport(
                QRhiViewport{ 0, 0, static_cast<float>(size.width()), static_cast<float>(size.height()) });
            command_buffer->setShaderResources();
            command_buffer->draw(3);
            command_buffer->endPass(readback);

            // Hand the texture back in COMMON for Avis's next write: a native transition, then tell Qt
            const int layout = texture_->nativeTexture().layout;
            state_.layout_after_draw = layout;
            if (layout != D3D12_RESOURCE_STATE_COMMON)
            {
                command_buffer->beginExternal();
                const D3D12_RESOURCE_BARRIER barrier{ .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                                      .Transition = {
                                                          .pResource = shared_texture_.Get(),
                                                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                          .StateBefore = static_cast<D3D12_RESOURCE_STATES>(layout),
                                                          .StateAfter = D3D12_RESOURCE_STATE_COMMON } };
                list->ResourceBarrier(1, &barrier);
                command_buffer->endExternal();
                texture_->setNativeLayout(D3D12_RESOURCE_STATE_COMMON);
            }
            state_.layout_handed_back = texture_->nativeTexture().layout;

            pending_marker_ = frame;
        }

    private:
        void import_shared()
        {
            imported_ = true;
            if (FAILED(device_->OpenSharedHandle(
                    static_cast<HANDLE>(state_.texture.value), IID_PPV_ARGS(&shared_texture_))) ||
                FAILED(
                    device_->OpenSharedHandle(static_cast<HANDLE>(state_.produced.value), IID_PPV_ARGS(&produced_))) ||
                FAILED(device_->OpenSharedHandle(static_cast<HANDLE>(state_.consumed.value), IID_PPV_ARGS(&consumed_))))
            {
                state_.fail("OpenSharedHandle failed on Qt's device");
                imported_ = false;
                state_.handles_ready = false;
                return;
            }

            texture_.reset(rhi()->newTexture(QRhiTexture::RGBA8, shared_size));
            if (!texture_->createFrom(
                    { static_cast<quint64>(reinterpret_cast<std::uintptr_t>(shared_texture_.Get())),
                      D3D12_RESOURCE_STATE_COMMON }))
            {
                state_.fail("QRhiTexture::createFrom refused the shared texture");
                imported_ = false;
                state_.handles_ready = false;
                return;
            }

            sampler_.reset(
                rhi()->newSampler(
                    QRhiSampler::Nearest,
                    QRhiSampler::Nearest,
                    QRhiSampler::None,
                    QRhiSampler::ClampToEdge,
                    QRhiSampler::ClampToEdge));
            sampler_->create();
            bindings_.reset(rhi()->newShaderResourceBindings());
            bindings_->setBindings(
                { QRhiShaderResourceBinding::sampledTexture(
                    0, QRhiShaderResourceBinding::FragmentStage, texture_.get(), sampler_.get()) });
            bindings_->create();

            const QShader vertex = load_shader(QStringLiteral("fullscreen_triangle.vert"));
            const QShader fragment = load_shader(QStringLiteral("fullscreen_texture.frag"));
            if (!vertex.isValid() || !fragment.isValid())
            {
                state_.fail("the Qt blit shaders are missing from the resources");
                imported_ = false;
                state_.handles_ready = false;
                return;
            }
            pipeline_.reset(rhi()->newGraphicsPipeline());
            pipeline_->setShaderStages(
                { { QRhiShaderStage::Vertex, vertex }, { QRhiShaderStage::Fragment, fragment } });
            pipeline_->setCullMode(QRhiGraphicsPipeline::None);
            pipeline_->setShaderResourceBindings(bindings_.get());
            pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!pipeline_->create())
            {
                state_.fail("the blit pipeline could not be created");
                imported_ = false;
                state_.handles_ready = false;
                return;
            }
            state_.imported = true;
        }

        void on_hook()
        {
            state_.check_thread = QThread::currentThreadId();
            if (pending_marker_ == 0 || state_.checked_frames >= frames_to_check)
            {
                return;
            }
            const std::uint64_t frame = std::exchange(pending_marker_, 0);

            // Everything submitted so far executes before `before_gate_` reaches `frame`; anything submitted later
            // waits behind `gate_`. So the marker reads `frame` exactly when Qt submitted this frame's command list
            // before the hook.
            queue_->Signal(before_gate_.Get(), frame);
            queue_->Wait(gate_.Get(), frame);
            queue_->Signal(consumed_.Get(), frame);

            before_gate_->SetEventOnCompletion(frame, event_);
            if (WaitForSingleObject(event_, gate_timeout_ms) != WAIT_OBJECT_0)
            {
                state_.fail(std::format("frame {}: the queue didn't reach the gate", frame));
            }
            else
            {
                const D3D12_RANGE read{ 0, sizeof(std::uint32_t) };
                void* mapped = nullptr;
                if (SUCCEEDED(marker_->Map(0, &read, &mapped)))
                {
                    const std::uint32_t marker = *static_cast<const std::uint32_t*>(mapped);
                    const D3D12_RANGE written{ 0, 0 };
                    marker_->Unmap(0, &written);
                    if (marker == frame)
                    {
                        ++state_.submitted_before;
                    }
                }
            }
            gate_->Signal(frame); // the CPU opens the gate
            state_.last_checked = frame;

            if (++state_.checked_frames == frames_to_check)
            {
                count_debug_errors();
            }
        }

        void on_readback()
        {
            const QByteArray& data = readback_.data;
            const QSize size = readback_.pixelSize;
            const qsizetype offset = (static_cast<qsizetype>(size.height() / 2) * size.width() + size.width() / 2) * 4;
            if (data.size() >= offset + 4)
            {
                for (int channel = 0; channel < 4; ++channel)
                {
                    state_.pixel[channel] = static_cast<unsigned char>(data[offset + channel]);
                }
            }
            state_.pixel_read = true;
        }

        void count_debug_errors()
        {
            if (!info_queue_)
            {
                return;
            }
            int errors = 0;
            const UINT64 count = info_queue_->GetNumStoredMessages();
            for (UINT64 index = 0; index < count; ++index)
            {
                SIZE_T length = 0;
                info_queue_->GetMessage(index, nullptr, &length);
                std::vector<std::byte> storage(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(info_queue_->GetMessage(index, message, &length)) &&
                    (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                     message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION))
                {
                    state_.fail(std::format("Qt's debug layer: {}", message->pDescription));
                    ++errors;
                }
            }
            state_.qt_debug_errors = errors;
        }

        interop_state& state_;
        QMetaObject::Connection hook_;

        ID3D12Device* device_ = nullptr; // Qt's
        ID3D12CommandQueue* queue_ = nullptr;
        ComPtr<ID3D12InfoQueue> info_queue_;
        ComPtr<ID3D12Fence> before_gate_;
        ComPtr<ID3D12Fence> gate_;
        ComPtr<ID3D12Resource> marker_;
        HANDLE event_ = nullptr;

        bool imported_ = false;
        ComPtr<ID3D12Resource> shared_texture_;
        ComPtr<ID3D12Fence> produced_;
        ComPtr<ID3D12Fence> consumed_;
        std::unique_ptr<QRhiTexture> texture_;
        std::unique_ptr<QRhiSampler> sampler_;
        std::unique_ptr<QRhiShaderResourceBindings> bindings_;
        std::unique_ptr<QRhiGraphicsPipeline> pipeline_;

        std::uint64_t frame_ = 0;
        std::uint64_t pending_marker_ = 0; // the frame render() recorded, until the hook checks it
        bool readback_requested_ = false;
        QRhiReadbackResult readback_;
    };

    class interop_item : public QQuickRhiItem
    {
    public:
        interop_item(interop_state& state, QQuickItem* parent) : QQuickRhiItem(parent), state_(state) {}

    protected:
        QQuickRhiItemRenderer* createRenderer() override
        {
            return new interop_renderer(state_);
        }

    private:
        interop_state& state_;
    };

    // The Avis side: the RHI on Qt's adapter, one shared texture cleared to avis_colour and left in COMMON, and the
    // shared fences. `produced` is 1 once the clear has been submitted.
    struct avis_side
    {
        std::unique_ptr<rhi::render_hardware_interface> hardware_interface;
        rhi::texture_handle texture;
        bool has_texture = false;
        std::optional<rhi::shared_fence> produced;
        std::optional<rhi::shared_fence> consumed;

        ~avis_side()
        {
            if (hardware_interface)
            {
                hardware_interface->wait_idle();
                produced.reset();
                consumed.reset();
                if (has_texture)
                {
                    rhi::resources{ *hardware_interface }.destroy(texture);
                }
            }
        }
    };

    std::optional<std::string> create_avis_side(avis_side& side, const rhi::adapter_luid adapter, interop_state& state)
    {
        auto created = rhi::render_hardware_interface::create(
            { .adapter = { .adapter = adapter }, .debug = { .debug_layer = true } });
        if (!created)
        {
            return "the Avis RHI could not be created on Qt's adapter: " + message_of(created.error());
        }
        side.hardware_interface = std::move(*created);
        rhi::render_hardware_interface& hardware_interface = *side.hardware_interface;
        if (hardware_interface.capabilities().luid != adapter)
        {
            return "the Avis RHI isn't on Qt's adapter";
        }

        rhi::sharing sharing{ hardware_interface };
        auto texture = sharing.create_shared_texture(
            { .width = static_cast<std::uint32_t>(shared_size.width()),
              .height = static_cast<std::uint32_t>(shared_size.height()),
              .format = rhi::format::rgba8_unorm,
              .usage =
                  rhi::texture_usage::shader_resource | rhi::texture_usage::render_target | rhi::texture_usage::shared,
              .initial_layout = rhi::texture_layout::render_target,
              .debug_name = "interop spike frame" });
        if (!texture)
        {
            return "create_shared_texture: " + message_of(texture.error());
        }
        side.texture = *texture;
        side.has_texture = true;

        auto produced = sharing.create_shared_fence();
        auto consumed = sharing.create_shared_fence();
        if (!produced || !consumed)
        {
            return "create_shared_fence failed";
        }
        side.produced.emplace(std::move(*produced));
        side.consumed.emplace(std::move(*consumed));

        auto list = rhi::command_lists{ hardware_interface }.acquire<rhi::queue_type::graphics>("interop spike clear");
        if (!list)
        {
            return "acquire: " + message_of(list.error());
        }
        list->clear_render_target(rhi::texture_view{ .texture = side.texture }, avis_colour);
        const std::array barriers{ rhi::texture_barrier{ .texture = side.texture,
                                                         .before = rhi::access_states::render_target,
                                                         .after = rhi::access_states::common } };
        list->barrier({}, {}, barriers);
        rhi::queue<rhi::queue_type::graphics> queue{ hardware_interface };
        if (auto submitted = queue.submit(std::span{ &*list, 1 }); !submitted)
        {
            return "submit: " + message_of(submitted.error());
        }
        queue.signal(*side.produced, 1);

        auto texture_handle = sharing.export_shared_handle(side.texture);
        auto produced_handle = sharing.export_shared_handle(*side.produced);
        auto consumed_handle = sharing.export_shared_handle(*side.consumed);
        if (!texture_handle || !produced_handle || !consumed_handle)
        {
            return "export_shared_handle failed";
        }
        state.texture = *texture_handle;
        state.produced = *produced_handle;
        state.consumed = *consumed_handle;
        state.handles_ready = true;
        return std::nullopt;
    }

    class qt_interop_test : public QObject
    {
        Q_OBJECT

    private Q_SLOTS:
        void initTestCase()
        {
            // Before the first window: the render loop and graphics API are chosen once
            qputenv("QSG_RENDER_LOOP", "threaded");
            qputenv("QSG_RHI_DEBUG_LAYER", "1");
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D12);
        }

        // 12 §3.3 rule 4's direct path: a Signal(consumed, m) from afterFrameEnd follows Qt's submission of frame m
        void after_frame_end_follows_submission()
        {
            interop_state state;
            run(state);
            if (QTest::currentTestFailed())
            {
                return;
            }

            QCOMPARE(state.submitted_before.load(), frames_to_check);

            // createFrom in COMMON, and Qt sampled what Avis wrote
            QVERIFY(state.imported.load());
            QVERIFY(state.pixel_read.load());
            for (int channel = 0; channel < 4; ++channel)
            {
                QVERIFY2(
                    std::abs(state.pixel[channel] - expected_pixel[channel]) <= 1,
                    std::format(
                        "pixel channel {}: {} instead of {}", channel, state.pixel[channel], expected_pixel[channel])
                        .c_str());
            }

            // Qt tracks the texture in a sampling state after its pass; setNativeLayout hands it back in COMMON
            qInfo("Qt's tracked state after sampling: 0x%x", state.layout_after_draw.load());
            QVERIFY(state.layout_after_draw.load() != D3D12_RESOURCE_STATE_COMMON);
            QCOMPARE(state.layout_handed_back.load(), static_cast<int>(D3D12_RESOURCE_STATE_COMMON));
            QCOMPARE(state.qt_debug_errors.load(), 0);
        }

        // The control: from afterRendering, before Qt's submission, the same check sees no frame submitted
        void after_rendering_precedes_submission()
        {
            interop_state state;
            state.hook = check_signal::after_rendering;
            run(state);
            if (QTest::currentTestFailed())
            {
                return;
            }

            QCOMPARE(state.submitted_before.load(), 0);
            QCOMPARE(state.qt_debug_errors.load(), 0);
        }

    private:
        // Shows a window with the item on Qt's threaded render loop, creates the Avis side on Qt's adapter, and lets
        // the hook check frames_to_check frames. Checks what both tests share: no render-thread failure, the threaded
        // loop with the hook on its render thread, and `consumed` reaching the last checked frame.
        static void run(interop_state& state)
        {
            // The Avis RHI's errors, its debug layer's included; attached before the RHI exists and outliving it
            const tests::log_capture avis_errors{ "rhi", 1024, spdlog::level::err };
            avis_side avis;
            std::uint64_t consumed_value = 0;
            {
                QQuickWindow window;
                window.resize(window_size);
                auto* item = new interop_item(state, window.contentItem());
                item->setSize(window_size);
                // One item update per frame Qt presents, so the item renders in every frame
                QObject::connect(&window, &QQuickWindow::frameSwapped, item, &QQuickItem::update);
                window.show();

                QTRY_VERIFY_WITH_TIMEOUT(state.adapter_known.load() || !state.failure_list().empty(), 10'000);
                QVERIFY2(state.failure_list().empty(), first_failure(state).c_str());

                const std::optional<std::string> failure = create_avis_side(avis, state.qt_adapter, state);
                QVERIFY2(!failure, failure ? failure->c_str() : "");
                item->update();

                QTRY_VERIFY_WITH_TIMEOUT(
                    state.checked_frames.load() >= frames_to_check || !state.failure_list().empty(), 30'000);
                QTRY_VERIFY_WITH_TIMEOUT(state.pixel_read.load() || !state.failure_list().empty(), 10'000);
                window.close();
                consumed_value = avis.consumed->completed_value();
            }

            const std::vector<std::string> failures = state.failure_list();
            for (const std::string& failure : failures)
            {
                qWarning("%s", failure.c_str());
            }
            QVERIFY2(failures.empty(), "the render thread reported failures (above)");
            for (const std::string& line : avis_errors.lines())
            {
                qWarning("Avis RHI: %s", line.c_str());
            }
            QVERIFY2(avis_errors.lines().empty(), "the Avis RHI logged errors (above)");

            QVERIFY(state.render_thread.load() != nullptr);
            QVERIFY(state.render_thread.load() != QThread::currentThreadId());
            QCOMPARE(state.check_thread.load(), state.render_thread.load());
            QCOMPARE(state.checked_frames.load(), frames_to_check);
            QVERIFY(consumed_value >= state.last_checked.load());
        }
    };
} // namespace

std::unique_ptr<QObject> avis::visualizer_ui_tests::make_qt_interop_test()
{
    return std::make_unique<qt_interop_test>();
}

#include "test_qt_interop.moc"
