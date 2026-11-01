#ifndef AVIS_SCENE_ASSETS_WIC_DECODER_H
#define AVIS_SCENE_ASSETS_WIC_DECODER_H

// PNG and JPEG decoding with WIC for the glTF import (docs/design/08-scene-management.md §5.1, §5.3). Private: only
// wic_decoder.cpp includes <windows.h> and <wincodec.h>.

#include "avis/error.h"
#include "avis/scene/assets/texture_data.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace avis::scene::detail
{
    // Decodes the first frame into tightly packed RGBA8: rgba8_srgb for colour textures, rgba8_unorm for data
    // textures, with generate_mips set (the renderer makes the mips, §5.3). A failure is image_decode_failed, with
    // `description` (the image and the file) in the message.
    //
    // Runs on any thread. COM is initialised once per thread (multithreaded apartment) on first use, and uninitialised
    // when the thread ends; a thread already in a single-threaded apartment keeps it.
    result<texture_data> decode_image(std::span<const std::byte> bytes, bool srgb, std::string_view description);
} // namespace avis::scene::detail

#endif
