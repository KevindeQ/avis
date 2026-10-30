#ifndef AVIS_GRAPHICS_SHADER_LOADING_H
#define AVIS_GRAPHICS_SHADER_LOADING_H

// Loading a shader library from a manifest and a blob directory (docs/design/18-graphics-shaders-and-pipelines.md §4).
// The only part of the shader library that depends on the IO layer; it needs no RHI, so a library can load while the
// device is created.

#include "avis/graphics/detail/shader_library_load_sender.h"
#include "avis/graphics/shader_library.h"
#include "avis/graphics/shader_manifest.h"

#include <filesystem>

namespace avis::io
{
    class io_context;
} // namespace avis::io

namespace avis::graphics
{
    // Reads every entry's blob (directory / entry.blob) at once, then on a parallel worker checks each content_hash
    // (xxh3) where present and calls shader_library::create, naming the library after the directory. The first failure
    // wins and stops the outstanding reads:
    // - invalid_argument, before any read, for a blob path that is absolute or has a ".." element;
    // - the IO error of a read, which names the path;
    // - shader_hash_mismatch naming the id, the file and both hashes;
    // - shader_library::create's errors.
    // A stop request stops the outstanding reads and completes with set_stopped, and so does a read stopped by its IO
    // context (the context is stopping).
    shader_library_load_sender
        load_shader_library(shader_manifest manifest, std::filesystem::path directory); // avis::runtime::io_context()
    shader_library_load_sender
        load_shader_library(io::io_context& context, shader_manifest manifest, std::filesystem::path directory);
} // namespace avis::graphics

#endif
