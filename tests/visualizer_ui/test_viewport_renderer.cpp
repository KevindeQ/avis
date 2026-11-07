// The viewport (docs/tasks/12-visualizer.md §12.5): ui::viewport_item and its renderer on Qt's real D3D12 device with
// the threaded render loop, fed by the shared frame ring of 12.4. The Avis side stands in for the frame loop (12.7):
// each frame it updates the rings, takes a slot from the gate, copies an upload into it, signals `produced`, publishes
// and wakes the item, as the frame chain will.

#include "helpers/log_capture.h"
#include "rendering/shared_frame_ring.h"
#include "ui/viewport_item.h"
#include "ui/viewport_renderer.h"
#include "ui_tests.h"

#include "avis/rhi/barriers.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/shared.h"

#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <stdexec/execution.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <rhi/qrhi.h>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

// QVERIFY2 for a helper's outcome: an empty string, or why it failed
#define AVIS_REQUIRE_DONE(expression)                         \
    do                                                        \
    {                                                         \
        const std::string avis_failure = (expression);        \
        QVERIFY2(avis_failure.empty(), avis_failure.c_str()); \
    }                                                         \
    while (false)

namespace
{
    using namespace avis;
    namespace rendering = avis::visualizer::rendering;
    namespace ui = avis::visualizer::ui;

    using rgba = std::array<std::uint8_t, 4>;

    constexpr QSize window_size{ 320, 240 };
    constexpr auto timeout = std::chrono::seconds{ 10 };

    constexpr rgba red{ 230, 20, 20, 255 };
    constexpr rgba blue{ 20, 40, 230, 255 };
    constexpr rgba green{ 30, 200, 60, 255 };

    rgba colour_of(const std::uint64_t frame)
    {
        return { static_cast<std::uint8_t>(frame * 37), static_cast<std::uint8_t>(255 - frame * 11), 128, 255 };
    }

    template<class Value>
    struct outcome
    {
        std::optional<Value> value;
        std::optional<avis::error> error;
        bool stopped = false;

        bool done() const noexcept
        {
            return value.has_value() || error.has_value() || stopped;
        }
    };

    template<class Value>
    struct outcome_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        template<class... Values>
        void set_value(Values&&... values) noexcept
        {
            result->value.emplace(std::forward<Values>(values)...);
        }

        void set_error(avis::error error) noexcept
        {
            result->error.emplace(std::move(error));
        }

        void set_stopped() noexcept
        {
            result->stopped = true;
        }

        auto get_env() const noexcept
        {
            return stdexec::prop{ stdexec::get_stop_token, token };
        }

        outcome<Value>* result;
        stdexec::inplace_stop_token token;
    };

    // A started sender with its outcome; not movable
    template<class Sender, class Value>
    struct started
    {
        explicit started(Sender sender) :
            operation{ stdexec::connect(std::move(sender), outcome_receiver<Value>{ &result, source.get_token() }) }
        {
            stdexec::start(operation);
        }

        outcome<Value> result;
        stdexec::inplace_stop_source source;
        stdexec::connect_result_t<Sender, outcome_receiver<Value>> operation;
    };

    // "" when `pixel` is `expected` within the rounding of a linear blit
    std::string compare_pixel(const QImage& image, const int x, const int y, const rgba expected)
    {
        const QColor pixel = image.pixelColor(x, y);
        const std::array actual{ pixel.red(), pixel.green(), pixel.blue() };
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            if (std::abs(actual[channel] - expected[channel]) > 2)
            {
                return std::format(
                    "pixel ({}, {}) of {}x{} is ({}, {}, {}), expected ({}, {}, {})",
                    x,
                    y,
                    image.width(),
                    image.height(),
                    actual[0],
                    actual[1],
                    actual[2],
                    expected[0],
                    expected[1],
                    expected[2]);
            }
        }
        return {};
    }

    // The viewport in a window on Qt's render thread, and the Avis side on Qt's adapter
    class viewport_fixture
    {
    public:
        viewport_fixture() = default;

        ~viewport_fixture()
        {
            // The renderer detaches when the window goes, then the rings and the uploads, then the RHI
            close_window();
            rings_.reset();
            if (hardware_interface_)
            {
                hardware_interface_->wait_idle();
                for (const rhi::buffer_handle buffer : uploads_)
                {
                    rhi::resources{ *hardware_interface_ }.destroy(buffer);
                }
            }
        }

        viewport_fixture(const viewport_fixture&) = delete;
        viewport_fixture& operator=(const viewport_fixture&) = delete;

        // Shows the window, creates the RHI on Qt's adapter and the rings, and waits for the first ring at the item's
        // pixel size
        std::string start()
        {
            window_ = std::make_unique<QQuickWindow>();
            window_->resize(window_size);
            item_ = new ui::viewport_item(window_->contentItem());
            item_->setSize(window_size);
            QObject::connect(
                window_.get(),
                &QQuickWindow::sceneGraphInitialized,
                window_.get(),
                [this]()
                {
                    const std::scoped_lock lock{ adapter_mutex_ };
                    adapter_ = ui::read_qt_adapter(*window_->rhi());
                    adapter_read_ = true;
                },
                Qt::DirectConnection);
            window_->show();

            std::optional<ui::qt_adapter> adapter;
            if (!pump_until(
                    [&]
                    {
                        const std::scoped_lock lock{ adapter_mutex_ };
                        adapter = adapter_;
                        return adapter_read_;
                    }))
            {
                return "Qt's scene graph didn't start";
            }
            if (!adapter)
            {
                return "Qt Quick isn't on Direct3D 12";
            }

            auto created = rhi::render_hardware_interface::create(
                { .adapter = { .adapter = adapter->luid }, .debug = { .debug_layer = true } });
            if (!created)
            {
                return "the Avis RHI could not be created on Qt's adapter: " + created.error().message;
            }
            hardware_interface_ = std::move(*created);
            driver_.emplace(*hardware_interface_);
            queue_.emplace(*hardware_interface_);
            rings_.emplace(rhi::sharing{ *hardware_interface_ }, rhi::resources{ *hardware_interface_ });

            item_->set_frame_rings(&*rings_);
            wake_ = item_->make_wake();

            // The item reports its size once Qt rendered it; the first report isn't debounced
            if (!pump_until([&] { return update_rings() != nullptr; }))
            {
                return "the item didn't report its pixel size";
            }
            return {};
        }

        // One frame as the frame chain runs it, with `top` in the upper half and `bottom` in the lower
        std::string produce(const rgba top, const rgba bottom)
        {
            rendering::shared_frame_ring* const ring = update_rings();
            if (ring == nullptr)
            {
                return "no ring";
            }

            started<rendering::shared_slot_sender, rendering::ring_slot> gate{ ring->shared_slot_free() };
            if (!pump_until([&] { return gate.result.done(); }) || !gate.result.value)
            {
                return std::format("frame {}: the gate gave no slot", frame_ + 1);
            }
            const std::uint32_t slot = gate.result.value->index();
            const std::uint64_t frame = ++frame_;
            if (std::string failure = write(*ring, std::move(*gate.result.value), frame, top, bottom); !failure.empty())
            {
                return failure;
            }

            started<rhi::gpu_wait_sender, std::monostate> produced{ ring->when_produced(frame) };
            if (!pump_until([&] { return produced.result.done(); }) || !produced.result.value)
            {
                return std::format("frame {} wasn't produced", frame);
            }
            picked_before_ = ring->protocol().picked(slot);
            if (ring->publish(frame, slot))
            {
                wake_();
            }
            last_ = { .frame = frame, .slot = slot };
            return {};
        }

        // Waits until Qt has drawn the last frame produced and its `consumed` signal has completed
        std::string wait_shown()
        {
            rendering::shared_frame_ring* const ring = rings_->current();
            const bool shown = pump_until(
                [&]
                {
                    const rendering::ring_protocol& protocol = ring->protocol();
                    const std::uint64_t use = protocol.picked(last_.slot);
                    return protocol.showing() == last_.slot && use > picked_before_ &&
                           ring->fence_values().consumed >= use;
                });
            return shown ? std::string{} : std::format("Qt didn't show frame {}", last_.frame);
        }

        // The window as Qt draws it, in device pixels
        QImage grab()
        {
            return window_->grabWindow();
        }

        // Pumps Qt's events and the RHI's completions until `done`, or the timeout
        bool pump_until(const std::function<bool()>& done)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!done())
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    return false;
                }
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                if (driver_)
                {
                    WaitForSingleObject(static_cast<HANDLE>(driver_->completion_event()), 2);
                    driver_->poll_completions();
                }
                else
                {
                    Sleep(2);
                }
            }
            return true;
        }

        // The frame chain's start of a frame: a pending size makes a new ring; released rings are destroyed
        rendering::shared_frame_ring* update_rings()
        {
            auto current = rings_->update();
            rings_->destroy_released();
            return current ? *current : nullptr;
        }

        void close_window()
        {
            item_ = nullptr;
            window_.reset();
        }

        QQuickWindow& window()
        {
            return *window_;
        }

        ui::viewport_item& item()
        {
            return *item_;
        }

        rendering::shared_frame_rings& rings()
        {
            return *rings_;
        }

        rendering::ring_frame last() const
        {
            return last_;
        }

        // Errors of the RHI (its info queue covers Qt's objects on the shared device, 12 §3.1) and of the viewport
        std::string logged_errors() const
        {
            std::string errors;
            for (const tests::log_capture* capture : { &rhi_errors_, &ui_errors_ })
            {
                for (const std::string& line : capture->lines())
                {
                    errors += line + "\n";
                }
            }
            return errors;
        }

    private:
        std::string write(
            rendering::shared_frame_ring& ring,
            rendering::ring_slot slot,
            const std::uint64_t frame,
            const rgba top,
            const rgba bottom)
        {
            rhi::resources resources{ *hardware_interface_ };
            const rhi::texture_handle texture = ring.texture(slot.index());
            const rhi::copy_footprint footprint = resources.copyable_footprint(texture, {});
            auto upload = resources.create_buffer(
                { .size = footprint.total_size,
                  .usage = rhi::buffer_usage::copy_source,
                  .memory = rhi::memory_type::upload,
                  .debug_name = "viewport test upload" });
            if (!upload)
            {
                return "create_buffer: " + upload.error().message;
            }
            uploads_.push_back(*upload);
            const std::span<std::byte> bytes = resources.mapped_bytes(*upload);
            for (std::uint32_t row = 0; row < footprint.height; ++row)
            {
                const rgba& colour = row < footprint.height / 2 ? top : bottom;
                for (std::uint32_t column = 0; column < footprint.width; ++column)
                {
                    std::memcpy(
                        bytes.data() + footprint.offset + row * footprint.row_pitch + column * 4, colour.data(), 4);
                }
            }

            auto list =
                rhi::command_lists{ *hardware_interface_ }.acquire<rhi::queue_type::graphics>("viewport test frame");
            if (!list)
            {
                return "acquire: " + list.error().message;
            }
            const std::array to_copy{ rhi::texture_barrier{ .texture = texture,
                                                            .before = rhi::access_states::common,
                                                            .after = rhi::access_states::copy_destination } };
            list->barrier({}, {}, to_copy);
            list->copy_buffer_to_texture(texture, {}, *upload, 0);
            const std::array to_common{ rhi::texture_barrier{ .texture = texture,
                                                              .before = rhi::access_states::copy_destination,
                                                              .after = rhi::access_states::common } };
            list->barrier({}, {}, to_common);
            if (auto submitted = queue_->submit(std::span{ &*list, 1 }); !submitted)
            {
                return "submit: " + submitted.error().message;
            }
            ring.signal_produced(*queue_, std::move(slot), frame);
            return {};
        }

        // Attached before the RHI exists and outliving it
        tests::log_capture rhi_errors_{ "rhi", 1024, spdlog::level::err };
        tests::log_capture ui_errors_{ "ui", 1024, spdlog::level::err };

        std::mutex adapter_mutex_;
        std::optional<ui::qt_adapter> adapter_;
        bool adapter_read_ = false;

        std::unique_ptr<rhi::render_hardware_interface> hardware_interface_;
        std::optional<rhi::completion_driver> driver_;
        std::optional<rhi::queue<rhi::queue_type::graphics>> queue_;
        std::optional<rendering::shared_frame_rings> rings_;
        std::vector<rhi::buffer_handle> uploads_;

        std::unique_ptr<QQuickWindow> window_;
        ui::viewport_item* item_ = nullptr; // owned by the window's content item
        std::function<void()> wake_;

        std::uint64_t frame_ = 0;
        rendering::ring_frame last_;
        std::uint64_t picked_before_ = 0; // Qt's last use of the last frame's slot before it was published
    };

    class viewport_renderer_test : public QObject
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

        // Rules 3-7: Qt draws the newest published frame, upright, and signals `consumed` for each pick, so the gate
        // keeps finding slots
        void shows_the_newest_frame()
        {
            viewport_fixture fixture;
            AVIS_REQUIRE_DONE(fixture.start());
            QCOMPARE(fixture.rings().current()->generation(), std::uint64_t{ 1 });
            QVERIFY(fixture.item().pixel_size().has_value());
            QVERIFY(fixture.rings().current()->size() == *fixture.item().pixel_size());

            AVIS_REQUIRE_DONE(fixture.produce(red, blue));
            AVIS_REQUIRE_DONE(fixture.wait_shown());
            QImage image = fixture.grab();
            AVIS_REQUIRE_DONE(compare_pixel(image, image.width() / 2, image.height() / 4, red));
            AVIS_REQUIRE_DONE(compare_pixel(image, image.width() / 2, image.height() * 3 / 4, blue));

            // Without Qt's `consumed` signals, the slots Qt picked would never come back to the gate
            for (std::uint64_t frame = 2; frame <= 60; ++frame)
            {
                AVIS_REQUIRE_DONE(fixture.produce(colour_of(frame), colour_of(frame)));
            }
            AVIS_REQUIRE_DONE(fixture.wait_shown());
            image = fixture.grab();
            AVIS_REQUIRE_DONE(compare_pixel(image, image.width() / 2, image.height() / 2, colour_of(60)));

            const std::string errors = fixture.logged_errors();
            QVERIFY2(errors.empty(), errors.c_str());
        }

        // Rule 8: a resize, debounced, makes one new ring; Qt moves to it once it has a frame, and the old ring is
        // released after Qt's detach
        void resize_moves_to_a_new_ring()
        {
            viewport_fixture fixture;
            AVIS_REQUIRE_DONE(fixture.start());
            AVIS_REQUIRE_DONE(fixture.produce(red, red));
            AVIS_REQUIRE_DONE(fixture.wait_shown());

            // Three sizes in quick succession: only the last holds for the debounce
            for (const QSize size : { QSize{ 400, 300 }, QSize{ 420, 310 }, QSize{ 480, 360 } })
            {
                fixture.window().resize(size);
                fixture.item().setSize(size);
                QTest::qWait(10);
            }
            rendering::shared_frame_ring* ring = nullptr;
            QVERIFY(fixture.pump_until(
                [&]
                {
                    ring = fixture.update_rings();
                    return ring != nullptr && ring->generation() > 1 && fixture.item().pixel_size() &&
                           ring->size() == *fixture.item().pixel_size();
                }));
            QCOMPARE(ring->generation(), std::uint64_t{ 2 });
            QCOMPARE(fixture.rings().retired().size(), std::size_t{ 1 });

            AVIS_REQUIRE_DONE(fixture.produce(green, blue));
            AVIS_REQUIRE_DONE(fixture.wait_shown());
            QVERIFY(fixture.pump_until(
                [&] { return fixture.update_rings() != nullptr && fixture.rings().retired().empty(); }));

            const QImage image = fixture.grab();
            AVIS_REQUIRE_DONE(compare_pixel(image, image.width() / 2, image.height() / 4, green));
            AVIS_REQUIRE_DONE(compare_pixel(image, image.width() / 2, image.height() * 3 / 4, blue));

            const std::string errors = fixture.logged_errors();
            QVERIFY2(errors.empty(), errors.c_str());
        }

        // Rule 6: with the window hidden Qt renders nothing, and Avis goes on through the slots Qt doesn't hold
        void hidden_window_does_not_stall_avis()
        {
            viewport_fixture fixture;
            AVIS_REQUIRE_DONE(fixture.start());
            AVIS_REQUIRE_DONE(fixture.produce(red, red));
            AVIS_REQUIRE_DONE(fixture.wait_shown());

            fixture.window().hide();
            QVERIFY(fixture.pump_until([&] { return !fixture.window().isExposed(); }));
            const rendering::ring_protocol& protocol = fixture.rings().current()->protocol();
            const std::optional<std::uint32_t> showing = protocol.showing();
            for (std::uint64_t frame = 0; frame < 30; ++frame)
            {
                AVIS_REQUIRE_DONE(fixture.produce(colour_of(frame), colour_of(frame)));
                QVERIFY(fixture.last().slot != showing);
            }

            fixture.window().show();
            AVIS_REQUIRE_DONE(fixture.produce(green, green));
            AVIS_REQUIRE_DONE(fixture.wait_shown());
            const QImage image = fixture.grab();
            AVIS_REQUIRE_DONE(compare_pixel(image, image.width() / 2, image.height() / 2, green));

            const std::string errors = fixture.logged_errors();
            QVERIFY2(errors.empty(), errors.c_str());
        }

        // The renderer's destruction detaches: a ring retired afterwards is released without Qt rendering again
        void closing_the_window_releases_the_ring()
        {
            viewport_fixture fixture;
            AVIS_REQUIRE_DONE(fixture.start());
            AVIS_REQUIRE_DONE(fixture.produce(red, red));
            AVIS_REQUIRE_DONE(fixture.wait_shown());
            const std::function<void()> wake = fixture.item().make_wake();

            fixture.close_window();
            QVERIFY(!fixture.rings().current()->protocol().showing().has_value());
            wake(); // the item is gone: the posted call does nothing
            QCoreApplication::processEvents();

            fixture.rings().request_resize({ .width = 64, .height = 32 });
            AVIS_REQUIRE_DONE(fixture.produce(blue, blue)); // creates ring 2, retires ring 1
            QCOMPARE(fixture.rings().current()->generation(), std::uint64_t{ 2 });
            QVERIFY(fixture.pump_until(
                [&] { return fixture.update_rings() != nullptr && fixture.rings().retired().empty(); }));

            const std::string errors = fixture.logged_errors();
            QVERIFY2(errors.empty(), errors.c_str());
        }
    };
} // namespace

std::unique_ptr<QObject> avis::visualizer_ui_tests::make_viewport_renderer_test()
{
    return std::make_unique<viewport_renderer_test>();
}

#include "test_viewport_renderer.moc"
