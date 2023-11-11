#ifndef CORE_STREAMS_MEMORY_STREAM_H
#define CORE_STREAMS_MEMORY_STREAM_H

#include "avis/core/common.h"

namespace streams
{
    enum class seek_origin
    {
        begin,
        current,
        end
    };

    class memory_stream
    {
    public:
        using value_type = unsigned char;
        using size_type = std::size_t;

    public:
        ENGINE_API memory_stream();
        ENGINE_API explicit memory_stream(size_type initial_stream_size);
        ENGINE_API explicit memory_stream(std::span<value_type> buffer);
        ENGINE_API memory_stream(std::span<value_type> buffer, size_type offset, size_type count);
        ENGINE_API memory_stream(const memory_stream& other);
        ENGINE_API memory_stream(memory_stream&& other) noexcept;
        ENGINE_API ~memory_stream();

        ENGINE_API void seek(std::int64_t offset, seek_origin origin);

        ENGINE_API size_type read(std::span<value_type> destination);
        ENGINE_API void write(std::span<value_type> source);

        ENGINE_API void copy_to(memory_stream& destination, size_type count);

        ENGINE_API void swap(memory_stream& other);

        ENGINE_API size_type offset() const
        {
            return offset_;
        }

        ENGINE_API size_type size() const
        {
            return size_;
        }

        ENGINE_API std::span<value_type> data() const
        {
            return std::span<value_type>{buffer_, size_};
        }

        ENGINE_API memory_stream& operator=(const memory_stream& other);
        ENGINE_API memory_stream& operator=(memory_stream&& other) noexcept;

    private:
        void resize_buffer(size_type new_size);

        void seek_from_begin(std::int64_t offset);
        void seek_from_current(std::int64_t offset);
        void seek_from_end(std::int64_t offset);

    private:
        value_type* buffer_;
        size_type size_;
        bool owning_buffer_;

        size_type offset_;
    };
} // namespace streams

#endif
