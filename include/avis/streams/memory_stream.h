#ifndef STREAMS_MEMORY_STREAM_H
#define STREAMS_MEMORY_STREAM_H

#include "avis/common.h"

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
        memory_stream();
        explicit memory_stream(size_type initial_stream_size);
        explicit memory_stream(std::span<value_type> buffer);
        memory_stream(std::span<value_type> buffer, size_type offset, size_type count);
        memory_stream(const memory_stream& other);
        memory_stream(memory_stream&& other) noexcept;
        ~memory_stream();

        void seek(std::int64_t offset, seek_origin origin);

        size_type read(std::span<value_type> destination);
        void write(std::span<value_type> source);

        void copy_to(memory_stream& destination, size_type count);

        void swap(memory_stream& other);

        size_type offset() const
        {
            return offset_;
        }

        size_type size() const
        {
            return size_;
        }

        std::span<value_type> data() const
        {
            return std::span<value_type>{buffer_, size_};
        }

        memory_stream& operator=(const memory_stream& other);
        memory_stream& operator=(memory_stream&& other) noexcept;

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
