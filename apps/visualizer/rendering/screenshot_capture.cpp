#include "rendering/screenshot_capture.h"

#include "avis/graphics/error.h"
#include "avis/io/file_operations.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <objbase.h>
#include <utility>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

namespace avis::visualizer::rendering
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        // The WIC factory and the multithreaded apartment it lives in, for the whole process, as the glTF import's
        // decoder does (lib/avis/scene/assets/wic_decoder.cpp): the parallel workers don't initialise COM, so the MTA
        // is kept alive with CoIncrementMTAUsage and the free-threaded factory is shared
        class process_wic
        {
        public:
            process_wic() noexcept
            {
                status_ = CoIncrementMTAUsage(&mta_usage_);
                if (FAILED(status_))
                {
                    mta_usage_ = nullptr;
                    return;
                }
                status_ = CoCreateInstance(
                    CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory_.GetAddressOf()));
            }

            ~process_wic()
            {
                factory_.Reset(); // before the MTA goes
                if (mta_usage_ != nullptr)
                {
                    CoDecrementMTAUsage(mta_usage_);
                }
            }

            process_wic(const process_wic&) = delete;
            process_wic& operator=(const process_wic&) = delete;

            HRESULT status() const noexcept
            {
                return status_;
            }

            IWICImagingFactory* factory() const noexcept
            {
                return factory_.Get();
            }

        private:
            ComPtr<IWICImagingFactory> factory_;
            CO_MTA_USAGE_COOKIE mta_usage_ = nullptr;
            HRESULT status_ = E_FAIL;
        };

        const process_wic& wic() noexcept
        {
            static const process_wic instance;
            return instance;
        }

        error wic_error(const std::string_view what, const HRESULT status)
        {
            // In the IO domain with the HRESULT: encoding is part of writing the file
            return error{ .domain = error_domain::io,
                          .code = static_cast<std::int32_t>(status),
                          .message =
                              std::format("{} failed (HRESULT 0x{:08X})", what, static_cast<std::uint32_t>(status)) };
        }

        error invalid(std::string message)
        {
            return graphics::make_error(graphics::error_kind::invalid_argument, std::move(message));
        }

        // ---- PNG chunks ----

        constexpr std::array<std::uint8_t, 8> png_signature{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

        constexpr std::array<std::uint32_t, 256> crc_table = []
        {
            std::array<std::uint32_t, 256> table{};
            for (std::uint32_t n = 0; n < 256; ++n)
            {
                std::uint32_t c = n;
                for (int k = 0; k < 8; ++k)
                {
                    c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                }
                table[n] = c;
            }
            return table;
        }();

        std::uint32_t crc32(const std::span<const std::byte> bytes) noexcept
        {
            std::uint32_t c = 0xFFFFFFFFu;
            for (const std::byte byte : bytes)
            {
                c = crc_table[(c ^ static_cast<std::uint8_t>(byte)) & 0xFFu] ^ (c >> 8);
            }
            return c ^ 0xFFFFFFFFu;
        }

        std::uint32_t read_big_endian(const std::span<const std::byte> bytes, const std::size_t offset) noexcept
        {
            return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
                   (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
                   (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | static_cast<std::uint32_t>(bytes[offset + 3]);
        }

        void append_big_endian(std::vector<std::byte>& bytes, const std::uint32_t value)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
            {
                bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
            }
        }

        bool is_png(const std::span<const std::byte> png) noexcept
        {
            return png.size() >= png_signature.size() &&
                   std::memcmp(png.data(), png_signature.data(), png_signature.size()) == 0;
        }

        // A PNG keyword: 1 to 79 printable Latin-1 characters, without leading, trailing or consecutive spaces
        bool valid_keyword(const std::string_view keyword) noexcept
        {
            if (keyword.empty() || keyword.size() > 79 || keyword.front() == ' ' || keyword.back() == ' ' ||
                keyword.find("  ") != std::string_view::npos)
            {
                return false;
            }
            return std::ranges::all_of(
                keyword,
                [](const char character)
                {
                    const auto code = static_cast<unsigned char>(character);
                    return (code >= 32 && code <= 126) || code >= 161;
                });
        }

        void append_text_chunk(std::vector<std::byte>& chunks, const png_text& text)
        {
            const std::size_t length = text.keyword.size() + 1 + text.text.size();
            append_big_endian(chunks, static_cast<std::uint32_t>(length));
            const std::size_t typed = chunks.size();
            for (const char character : std::string_view{ "tEXt" })
            {
                chunks.push_back(static_cast<std::byte>(character));
            }
            for (const char character : text.keyword)
            {
                chunks.push_back(static_cast<std::byte>(character));
            }
            chunks.push_back(std::byte{ 0 });
            for (const char character : text.text)
            {
                chunks.push_back(static_cast<std::byte>(character));
            }
            append_big_endian(chunks, crc32(std::span{ chunks }.subspan(typed)));
        }

        std::string camera_text(const camera& pose)
        {
            const Eigen::Vector3f position = pose.world_from_view.translation();
            const Eigen::Quaternionf rotation{ pose.world_from_view.rotation() };
            return std::format(
                "position {} {} {}; rotation (w x y z) {} {} {} {}; vertical field of view {} rad; near {} m",
                position.x(),
                position.y(),
                position.z(),
                rotation.w(),
                rotation.x(),
                rotation.y(),
                rotation.z(),
                pose.vertical_field_of_view,
                pose.near_plane);
        }

        // About what a view of one pixel holds without aliasing: depth, linear depth and its mips, the G-buffer,
        // hdr_color, the AO, the resolve, the accumulation, ldr_color, the TAA and depth histories, and the readback
        constexpr std::uint64_t screenshot_bytes_per_pixel = 128;
    } // namespace

    // ---- PNG ----

    std::vector<png_text> screenshot_text(const screenshot_metadata& metadata)
    {
        std::vector<png_text> text{ { .keyword = "Software", .text = "Avis" } };
        const auto add = [&text](std::string keyword, std::string value)
        {
            if (!value.empty())
            {
                text.push_back({ .keyword = std::move(keyword), .text = std::move(value) });
            }
        };
        add("Scenario", metadata.scenario);
        add("Step", std::to_string(metadata.step));
        add("Simulation time", std::format("{} s", metadata.simulation_time));
        add("Driver", metadata.driver_id);
        add("Camera", metadata.camera_pose);
        add("Render settings", metadata.render_settings);
        return text;
    }

    result<std::vector<std::byte>> encode_png(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::span<const std::byte> rgba,
        const std::uint32_t row_pitch,
        const std::span<const png_text> text)
    {
        if (width == 0 || height == 0 || row_pitch < std::uint64_t{ width } * 4 ||
            rgba.size() < std::uint64_t{ row_pitch } * (height - 1) + std::uint64_t{ width } * 4)
        {
            return std::unexpected{ invalid(
                std::format(
                    "encode_png: {} bytes don't hold a {} x {} image with rows {} bytes apart",
                    rgba.size(),
                    width,
                    height,
                    row_pitch)) };
        }
        for (const png_text& entry : text)
        {
            if (!valid_keyword(entry.keyword) || entry.text.find('\0') != std::string::npos)
            {
                return std::unexpected{ invalid(std::format("encode_png: invalid text chunk '{}'", entry.keyword)) };
            }
        }
        const process_wic& shared = wic();
        if (FAILED(shared.status()))
        {
            return std::unexpected{ wic_error("Creating the WIC factory", shared.status()) };
        }
        IWICImagingFactory* factory = shared.factory();

        // BGRA with alpha 255: ldr_color already holds sRGB-encoded bytes (11 §12.1)
        const std::uint32_t stride = width * 4;
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(stride) * height);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            const std::byte* source = rgba.data() + static_cast<std::size_t>(y) * row_pitch;
            std::uint8_t* destination = bgra.data() + static_cast<std::size_t>(y) * stride;
            for (std::uint32_t x = 0; x < width; ++x)
            {
                destination[x * 4 + 0] = static_cast<std::uint8_t>(source[x * 4 + 2]);
                destination[x * 4 + 1] = static_cast<std::uint8_t>(source[x * 4 + 1]);
                destination[x * 4 + 2] = static_cast<std::uint8_t>(source[x * 4 + 0]);
                destination[x * 4 + 3] = 255;
            }
        }

        ComPtr<IStream> stream;
        HRESULT status = CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
        if (FAILED(status))
        {
            return std::unexpected{ wic_error("CreateStreamOnHGlobal", status) };
        }
        ComPtr<IWICBitmapEncoder> encoder;
        status = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
        if (SUCCEEDED(status))
        {
            status = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        }
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        if (SUCCEEDED(status))
        {
            status = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
        }
        if (SUCCEEDED(status))
        {
            status = frame->Initialize(properties.Get());
        }
        if (SUCCEEDED(status))
        {
            status = frame->SetSize(width, height);
        }
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(status))
        {
            status = frame->SetPixelFormat(&format);
        }
        if (SUCCEEDED(status) && format != GUID_WICPixelFormat32bppBGRA)
        {
            return std::unexpected{ invalid("encode_png: the WIC PNG encoder doesn't take 32-bit BGRA") };
        }
        if (SUCCEEDED(status))
        {
            status = frame->WritePixels(height, stride, static_cast<UINT>(bgra.size()), bgra.data());
        }
        if (SUCCEEDED(status))
        {
            status = frame->Commit();
        }
        if (SUCCEEDED(status))
        {
            status = encoder->Commit();
        }
        STATSTG statistics{};
        if (SUCCEEDED(status))
        {
            status = stream->Stat(&statistics, STATFLAG_NONAME);
        }
        HGLOBAL memory = nullptr;
        if (SUCCEEDED(status))
        {
            status = GetHGlobalFromStream(stream.Get(), &memory);
        }
        if (FAILED(status))
        {
            return std::unexpected{ wic_error("Encoding the PNG", status) };
        }

        const auto size = static_cast<std::size_t>(statistics.cbSize.QuadPart);
        const void* encoded = GlobalLock(memory);
        if (encoded == nullptr)
        {
            return std::unexpected{ wic_error("GlobalLock", HRESULT_FROM_WIN32(GetLastError())) };
        }
        const std::span<const std::byte> png{ static_cast<const std::byte*>(encoded), size };

        // The text chunks after IHDR (the signature, then IHDR's length, type, 13 bytes of data and CRC)
        constexpr std::size_t ihdr_end = 8 + 4 + 4 + 13 + 4;
        if (!is_png(png) || png.size() < ihdr_end || read_big_endian(png, 8) != 13)
        {
            GlobalUnlock(memory);
            return std::unexpected{ invalid("encode_png: WIC wrote no IHDR chunk first") };
        }
        std::vector<std::byte> bytes;
        bytes.reserve(png.size() + 64 * text.size());
        bytes.insert(bytes.end(), png.begin(), png.begin() + ihdr_end);
        for (const png_text& entry : text)
        {
            append_text_chunk(bytes, entry);
        }
        bytes.insert(bytes.end(), png.begin() + ihdr_end, png.end());
        GlobalUnlock(memory);
        return bytes;
    }

    result<png_image> decode_png(const std::span<const std::byte> png)
    {
        const process_wic& shared = wic();
        if (FAILED(shared.status()))
        {
            return std::unexpected{ wic_error("Creating the WIC factory", shared.status()) };
        }
        if (png.size() > std::numeric_limits<DWORD>::max())
        {
            return std::unexpected{ invalid("decode_png: the PNG is too large") };
        }
        IWICImagingFactory* factory = shared.factory();
        ComPtr<IWICStream> stream;
        HRESULT status = factory->CreateStream(stream.GetAddressOf());
        if (SUCCEEDED(status))
        {
            // WIC only reads through the stream
            status = stream->InitializeFromMemory(
                reinterpret_cast<BYTE*>(const_cast<std::byte*>(png.data())), static_cast<DWORD>(png.size()));
        }
        ComPtr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(status))
        {
            status = factory->CreateDecoderFromStream(
                stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(status))
        {
            status = decoder->GetFrame(0, frame.GetAddressOf());
        }
        ComPtr<IWICBitmapSource> converted;
        if (SUCCEEDED(status))
        {
            status = WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), converted.GetAddressOf());
        }
        png_image image;
        if (SUCCEEDED(status))
        {
            status = converted->GetSize(&image.width, &image.height);
        }
        if (SUCCEEDED(status))
        {
            image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
            status = converted->CopyPixels(
                nullptr, image.width * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data());
        }
        if (FAILED(status))
        {
            return std::unexpected{ wic_error("Decoding the PNG", status) };
        }
        return image;
    }

    std::vector<png_text> read_png_text(const std::span<const std::byte> png)
    {
        std::vector<png_text> text;
        if (!is_png(png))
        {
            return text;
        }
        std::size_t offset = png_signature.size();
        while (offset + 12 <= png.size())
        {
            const std::uint32_t length = read_big_endian(png, offset);
            if (offset + 12 + std::uint64_t{ length } > png.size())
            {
                break;
            }
            const std::span<const std::byte> type = png.subspan(offset + 4, 4);
            const std::span<const std::byte> data = png.subspan(offset + 8, length);
            if (std::memcmp(type.data(), "tEXt", 4) == 0)
            {
                const auto separator = std::ranges::find(data, std::byte{ 0 });
                if (separator != data.end())
                {
                    const auto as_string = [](const auto first, const auto last)
                    {
                        std::string value;
                        for (auto it = first; it != last; ++it)
                        {
                            value.push_back(static_cast<char>(*it));
                        }
                        return value;
                    };
                    text.push_back(
                        { .keyword = as_string(data.begin(), separator),
                          .text = as_string(separator + 1, data.end()) });
                }
            }
            if (std::memcmp(type.data(), "IEND", 4) == 0)
            {
                break;
            }
            offset += 12 + length;
        }
        return text;
    }

    // ---- Requests ----

    std::uint64_t screenshot_transient_bytes(const std::uint32_t width, const std::uint32_t height) noexcept
    {
        return std::uint64_t{ width } * height * screenshot_bytes_per_pixel;
    }

    std::optional<std::string>
        screenshot_refusal(const screenshot_request& request, const std::optional<rhi::memory_budget> budget)
    {
        if (request.path.empty())
        {
            return "no path to write to";
        }
        if (request.mode == screenshot_mode::viewport)
        {
            return std::nullopt;
        }
        if (request.width == 0 || request.height == 0 || request.width > maximum_screenshot_width ||
            request.height > maximum_screenshot_height)
        {
            return std::format(
                "{} x {} is outside 1 x 1 to {} x {}",
                request.width,
                request.height,
                maximum_screenshot_width,
                maximum_screenshot_height);
        }
        if (budget)
        {
            const std::uint64_t free = budget->budget > budget->usage ? budget->budget - budget->usage : 0;
            const std::uint64_t estimate = screenshot_transient_bytes(request.width, request.height);
            if (static_cast<double>(estimate) > screenshot_budget_fraction * static_cast<double>(free))
            {
                return std::format(
                    "{} x {} needs about {} MiB of video memory, more than half of the {} MiB free",
                    request.width,
                    request.height,
                    estimate >> 20,
                    free >> 20);
            }
        }
        return std::nullopt;
    }

    screenshot_capture::screenshot_capture(const rhi::resources resources, const rhi::synchronization synchronization) :
        resources_{ resources },
        synchronization_{ synchronization }
    {}

    screenshot_capture::~screenshot_capture()
    {
        stdexec::sync_wait(writes_.on_empty());
        if (job_)
        {
            release(*job_);
        }
    }

    void screenshot_capture::request(screenshot_request request, const std::optional<rhi::memory_budget> budget)
    {
        if (const std::optional<std::string> refusal = screenshot_refusal(request, budget))
        {
            report(
                { .id = request.id,
                  .path = request.path,
                  .failure = invalid(std::format("screenshot {} refused: {}", request.id, *refusal)) });
            return;
        }
        if (pending_)
        {
            report(
                { .id = pending_->id,
                  .path = pending_->path,
                  .failure = make_error(
                      generic_error_kind::cancelled,
                      std::format("screenshot {} superseded by {} before it started", pending_->id, request.id)) });
        }
        pending_ = std::move(request);
    }

    screenshot_job* screenshot_capture::begin_frame(
        const std::uint64_t frame_index,
        const scene::snapshot& snapshot,
        const std::chrono::steady_clock::time_point wall_now,
        const std::span<const view_description> views)
    {
        poll();
        if (job_ && job_->state != screenshot_job::phase::rendering)
        {
            job_->snapshot.reset(); // the frame of the last sub-frame has executed
        }
        if (!job_ && pending_)
        {
            job_.emplace();
            job_->request = std::move(*pending_);
            pending_.reset();
            written_.store(false, std::memory_order_relaxed);
            job_->first_frame_index = frame_index;
            if (job_->custom())
            {
                job_->snapshot = snapshot;
                job_->wall_time = wall_now;
            }
            if (auto started = start(*job_, views); !started)
            {
                fail(*job_, std::move(started).error());
            }
        }
        return job_ && job_->state == screenshot_job::phase::rendering ? &*job_ : nullptr;
    }

    result<void> screenshot_capture::start(screenshot_job& job, const std::span<const view_description> views)
    {
        const auto view = std::ranges::find(views, job.request.view, &view_description::name);
        if (view == views.end())
        {
            return std::unexpected{ invalid(
                std::format("screenshot {}: the frame has no view '{}'", job.request.id, job.request.view)) };
        }
        job.camera = view->camera;
        job.device_pixel_ratio = view->device_pixel_ratio;
        if (!job.custom())
        {
            return {};
        }
        if (std::ranges::contains(views, screenshot_view_name, &view_description::name))
        {
            return std::unexpected{ invalid(
                std::format(
                    "screenshot {}: a view of the frame is already named '{}'",
                    job.request.id,
                    screenshot_view_name)) };
        }
        if (job.request.include_debug_blit)
        {
            job.debug_blits = view->options.debug_blits;
        }
        auto accumulation = resources_.create_texture(
            { .width = job.request.width,
              .height = job.request.height,
              .format = rhi::format::rgba32_float,
              .usage = rhi::texture_usage::shader_resource | rhi::texture_usage::unordered_access,
              .initial_layout = rhi::texture_layout::common,
              .debug_name = "screenshot_accumulation" });
        if (!accumulation)
        {
            return std::unexpected{ std::move(accumulation).error() };
        }
        job.accumulation = *accumulation;
        return create_viewport_readback(job, { job.request.width, job.request.height });
    }

    result<void> screenshot_capture::create_viewport_readback(screenshot_job& job, const graphics::texture_extent size)
    {
        // copy_texture_to_buffer's footprint: rows 256-byte aligned
        constexpr std::uint32_t pitch_alignment = 256;
        job.size = size;
        job.row_pitch = (size.width * 4 + pitch_alignment - 1) / pitch_alignment * pitch_alignment;
        auto buffer = resources_.create_buffer(
            { .size = std::uint64_t{ job.row_pitch } * size.height,
              .usage = rhi::buffer_usage::copy_destination,
              .memory = rhi::memory_type::readback,
              .debug_name = "screenshot_readback" });
        if (!buffer)
        {
            return std::unexpected{ std::move(buffer).error() };
        }
        job.readback = *buffer;
        return {};
    }

    void screenshot_capture::submitted(const std::uint64_t frame_index, const graphics::submitted_frame& frame)
    {
        if (job_ && job_->state == screenshot_job::phase::reading && job_->last_frame_index == frame_index &&
            !job_->submitted)
        {
            job_->submitted = frame;
        }
    }

    void screenshot_capture::frame_failed(const std::uint64_t frame_index) noexcept
    {
        if (!job_ || frame_index < job_->first_frame_index)
        {
            return;
        }
        const bool rendering = job_->state == screenshot_job::phase::rendering;
        const bool copying = job_->state == screenshot_job::phase::reading && !job_->submitted;
        if (rendering || copying)
        {
            error failure;
            try
            {
                failure = graphics::make_error(
                    graphics::error_kind::invalid_argument,
                    std::format("screenshot {}: frame {} that rendered it failed", job_->request.id, frame_index));
            }
            catch (...)
            {
                // Out of memory: the result says less
            }
            fail(*job_, std::move(failure));
        }
    }

    void screenshot_capture::poll()
    {
        if (!job_)
        {
            return;
        }
        if (job_->state == screenshot_job::phase::writing)
        {
            if (written_.load(std::memory_order_acquire))
            {
                job_.reset();
            }
            return;
        }
        if (job_->state != screenshot_job::phase::reading || !job_->submitted)
        {
            return;
        }
        const bool complete = std::ranges::all_of(
            job_->submitted->last_submissions(),
            [this](const rhi::synchronization_point point) { return synchronization_.is_complete(point); });
        if (!complete)
        {
            return;
        }

        // The pixels leave the readback buffer now: the worker owns its copy
        const std::span<const std::byte> mapped = resources_.mapped_bytes(job_->readback);
        std::vector<std::byte> pixels{ mapped.begin(), mapped.end() };
        release(*job_);
        job_->snapshot.reset();
        job_->state = screenshot_job::phase::writing;

        screenshot_metadata metadata = job_->request.metadata;
        if (metadata.camera_pose.empty())
        {
            metadata.camera_pose = camera_text(job_->camera);
        }
        const screenshot_result base{ .id = job_->request.id,
                                      .path = job_->request.path,
                                      .size = job_->size,
                                      .first_frame_index = job_->first_frame_index,
                                      .last_frame_index = job_->last_frame_index };
        auto work =
            stdexec::schedule(stdexec::get_parallel_scheduler()) |
            stdexec::then(
                [pixels = std::move(pixels),
                 size = job_->size,
                 row_pitch = job_->row_pitch,
                 text = screenshot_text(metadata)]()
                {
                    auto png = encode_png(size.width, size.height, pixels, row_pitch, text);
                    if (!png)
                    {
                        throw avis::runtime_error{ std::move(png).error() };
                    }
                    return std::move(*png);
                }) |
            stdexec::let_value([path = base.path](std::vector<std::byte>& png)
                               { return io::write_file(path, std::move(png), io::write_policy::atomic_replace); }) |
            stdexec::then(
                [this, base]() noexcept
                {
                    report(base);
                    written_.store(true, std::memory_order_release);
                }) |
            stdexec::upon_error(
                [this, base]<typename Error>(Error&& failure) noexcept
                {
                    screenshot_result failed = base;
                    failed.failure = threading::detail::to_internal_error(std::forward<Error>(failure));
                    report(std::move(failed));
                    written_.store(true, std::memory_order_release);
                }) |
            stdexec::upon_stopped(
                [this, base]() noexcept
                {
                    screenshot_result failed = base;
                    failed.failure = make_error(generic_error_kind::cancelled, "the screenshot's write was stopped");
                    report(std::move(failed));
                    written_.store(true, std::memory_order_release);
                });
        writes_.spawn(std::move(work));
    }

    std::vector<screenshot_result> screenshot_capture::take_results()
    {
        poll();
        const std::scoped_lock lock{ results_mutex_ };
        return std::exchange(results_, {});
    }

    void screenshot_capture::report(screenshot_result result)
    {
        try
        {
            const std::scoped_lock lock{ results_mutex_ };
            results_.push_back(std::move(result));
        }
        catch (...)
        {
            // Out of memory: the result is lost, the queue goes on
        }
    }

    void screenshot_capture::fail(screenshot_job& job, error failure)
    {
        report(
            { .id = job.request.id,
              .path = job.request.path,
              .failure = std::move(failure),
              .size = job.size,
              .first_frame_index = job.first_frame_index,
              .last_frame_index = job.last_frame_index });
        release(job);
        job_.reset();
    }

    void screenshot_capture::release(screenshot_job& job) noexcept
    {
        // Deferred by the RHI until the GPU is done with them
        if (job.readback.is_valid())
        {
            resources_.destroy(job.readback);
            job.readback = {};
        }
        if (job.accumulation.is_valid())
        {
            resources_.destroy(job.accumulation);
            job.accumulation = {};
        }
    }
} // namespace avis::visualizer::rendering
