#include "wic_decoder.h"

#include "avis/scene/error.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <cstdint>
#include <format>
#include <limits>
#include <objbase.h>
#include <string>
#include <vector>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

namespace avis::scene::detail
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        // Larger than any texture the renderer accepts (D3D12 allows 16384), and small enough that the RGBA8 size of
        // the image fits in a 32-bit WIC buffer size
        constexpr std::uint32_t maximum_dimension = 16384;

        // The WIC factory, shared by every decoding thread, and the multithreaded apartment it lives in.
        //
        // The parallel scheduler's workers don't initialise COM. Instead of each thread initialising it, the MTA is
        // kept alive for the process with CoIncrementMTAUsage, so the workers belong to the implicit MTA and can use
        // the factory, which is free-threaded. A thread_local guard can't be used: its destructor runs from the
        // exe's TLS callback at thread exit, after combase's own thread cleanup and under the loader lock, where
        // releasing the factory or calling CoUninitialize is not allowed and crashes.
        //
        // The destructor runs during static destruction, after every pool has been joined.
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

        const process_wic& shared_wic() noexcept
        {
            static const process_wic wic;
            return wic;
        }

        avis::error decode_failure(const std::string_view description, const std::string_view step, const HRESULT code)
        {
            return make_error(
                error_kind::image_decode_failed,
                std::format("{}: {} failed (HRESULT 0x{:08x})", description, step, static_cast<std::uint32_t>(code)));
        }
    } // namespace

    result<texture_data>
        decode_image(const std::span<const std::byte> bytes, const bool srgb, const std::string_view description)
    {
        if (bytes.empty())
        {
            return std::unexpected(
                make_error(error_kind::image_decode_failed, std::format("{}: no data", description)));
        }
        if (bytes.size() > std::numeric_limits<DWORD>::max())
        {
            return std::unexpected(
                make_error(error_kind::image_decode_failed, std::format("{}: larger than 4 GiB", description)));
        }

        const process_wic& wic = shared_wic();
        if (FAILED(wic.status()))
        {
            return std::unexpected(decode_failure(description, "creating the WIC factory", wic.status()));
        }
        IWICImagingFactory* const factory = wic.factory();

        ComPtr<IWICStream> stream;
        HRESULT code = factory->CreateStream(stream.GetAddressOf());
        if (FAILED(code))
        {
            return std::unexpected(decode_failure(description, "CreateStream", code));
        }
        // WIC only reads the memory; the parameter just isn't const
        code = stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())), static_cast<DWORD>(bytes.size()));
        if (FAILED(code))
        {
            return std::unexpected(decode_failure(description, "InitializeFromMemory", code));
        }

        ComPtr<IWICBitmapDecoder> decoder;
        code = factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
        if (FAILED(code))
        {
            return std::unexpected(decode_failure(description, "CreateDecoderFromStream", code));
        }

        ComPtr<IWICBitmapFrameDecode> frame;
        code = decoder->GetFrame(0, frame.GetAddressOf());
        if (FAILED(code))
        {
            return std::unexpected(decode_failure(description, "GetFrame", code));
        }

        UINT width = 0;
        UINT height = 0;
        code = frame->GetSize(&width, &height);
        if (FAILED(code))
        {
            return std::unexpected(decode_failure(description, "GetSize", code));
        }
        if (width == 0 || height == 0 || width > maximum_dimension || height > maximum_dimension)
        {
            return std::unexpected(make_error(
                error_kind::image_decode_failed,
                std::format(
                    "{}: {} x {} pixels is outside 1 to {} per side", description, width, height, maximum_dimension)));
        }

        // Straight (not premultiplied) alpha, as glTF expects. No colour management: the bytes are kept as they are,
        // and the format says how to read them.
        ComPtr<IWICFormatConverter> converter;
        code = factory->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(code))
        {
            return std::unexpected(decode_failure(description, "CreateFormatConverter", code));
        }
        code = converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
        if (FAILED(code))
        {
            return std::unexpected(decode_failure(description, "converting to RGBA8", code));
        }

        const UINT stride = width * 4;
        const UINT size = stride * height; // at most 1 GiB: maximum_dimension² × 4
        std::vector<std::byte> pixels(size);
        code = converter->CopyPixels(nullptr, stride, size, reinterpret_cast<BYTE*>(pixels.data()));
        if (FAILED(code))
        {
            return std::unexpected(decode_failure(description, "CopyPixels", code));
        }

        texture_data decoded;
        decoded.width = width;
        decoded.height = height;
        decoded.format = srgb ? texture_format::rgba8_srgb : texture_format::rgba8_unorm;
        decoded.generate_mips = true;
        decoded.mips.push_back(std::move(pixels));
        return decoded;
    }
} // namespace avis::scene::detail
