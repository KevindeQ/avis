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

        // COM and the WIC factory for the calling thread. The parallel scheduler's workers don't initialise COM, so the
        // first decode on a thread does, and the thread_local's destructor undoes it when the thread ends.
        class thread_wic
        {
        public:
            thread_wic() noexcept
            {
                const HRESULT initialised = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                // S_FALSE: already initialised in this apartment, which still needs the matching CoUninitialize.
                // RPC_E_CHANGED_MODE: the thread is in a single-threaded apartment, where WIC works too; don't
                // uninitialise what isn't ours.
                uninitialise_ = SUCCEEDED(initialised);
                if (FAILED(initialised) && initialised != RPC_E_CHANGED_MODE)
                {
                    status_ = initialised;
                    return;
                }
                status_ = CoCreateInstance(
                    CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory_.GetAddressOf()));
            }

            ~thread_wic()
            {
                factory_.Reset(); // before COM goes
                if (uninitialise_)
                {
                    CoUninitialize();
                }
            }

            thread_wic(const thread_wic&) = delete;
            thread_wic& operator=(const thread_wic&) = delete;

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
            HRESULT status_ = E_FAIL;
            bool uninitialise_ = false;
        };

        thread_wic& wic_for_this_thread() noexcept
        {
            thread_local thread_wic wic;
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

        const thread_wic& wic = wic_for_this_thread();
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
