#include "avis/core/streams/memory_stream.h"

#include "avis/core/common.h"

namespace streams
{
    memory_stream::memory_stream() : buffer_{nullptr}, size_{0}, owning_buffer_{true}, offset_{0} {}

    memory_stream::memory_stream(size_type initial_stream_size) :
        buffer_{nullptr},
        size_{0},
        owning_buffer_{true},
        offset_{0}
    {
        value_type* temp_buffer = new value_type[initial_stream_size];

        buffer_ = temp_buffer;
        size_ = initial_stream_size;
    }

    memory_stream::memory_stream(std::span<value_type> buffer) :
        buffer_{buffer.data()},
        size_{buffer.size()},
        owning_buffer_{false},
        offset_{0}
    {}

    memory_stream::memory_stream(std::span<value_type> buffer, size_type offset, size_type count) :
        buffer_{buffer.data() + offset},
        size_{count},
        owning_buffer_{false},
        offset_{0}
    {}

    memory_stream::memory_stream(const memory_stream& other) :
        buffer_{nullptr},
        size_{0},
        owning_buffer_{true},
        offset_{0}
    {
        value_type* temp_buffer = other.buffer_;
        if (other.owning_buffer_)
        {
            temp_buffer = new value_type[other.size_];
            std::copy(other.buffer_, other.buffer_ + other.size_, temp_buffer);
        }

        buffer_ = temp_buffer;
        size_ = other.size_;
        owning_buffer_ = other.owning_buffer_;

        offset_ = other.offset_;
    }

    memory_stream::memory_stream(memory_stream&& other) noexcept :
        buffer_{std::move(other.buffer_)},
        size_{std::move(other.size_)},
        owning_buffer_{std::move(other.owning_buffer_)},

        offset_{std::move(other.offset_)}
    {
        other.buffer_ = nullptr;
        other.size_ = 0;
        other.owning_buffer_ = true;

        other.offset_ = 0;
    }

    memory_stream::~memory_stream()
    {
        if (owning_buffer_)
        {
            delete[] buffer_;
        }
    }

    void memory_stream::seek(std::int64_t offset, seek_origin origin)
    {
        switch (origin)
        {
        case seek_origin::begin:
            seek_from_begin(offset);
            break;
        case seek_origin::end:
            seek_from_end(offset);
            break;

            [[fallthrough]];
        case seek_origin::current:
        default:
            seek_from_current(offset);
            break;
        }
    }

    memory_stream::size_type memory_stream::read(std::span<value_type> destination)
    {
        size_type max_copy_byte_count = std::min(destination.size(), size_ - offset_);

        value_type* source = buffer_ + offset_;
        std::copy(source, source + max_copy_byte_count, destination.data());

        offset_ += max_copy_byte_count;

        return max_copy_byte_count;
    }

    void memory_stream::write(std::span<value_type> source)
    {
        size_type remaining_buffer_size = size_ - offset_;
        if (source.size() > remaining_buffer_size)
        {
            resize_buffer(size_ + source.size() - remaining_buffer_size);
        }

        std::copy(source.data(), source.data() + source.size(), buffer_ + offset_);

        offset_ += source.size();
    }

    void memory_stream::copy_to(memory_stream& destination, size_type count)
    {
        // Compute the number of available bytes for copy in the source buffer
        size_type max_copy_byte_count = std::min(size_ - offset_, count);

        // Check if the destination buffer is large enough to copy the source too and resize the destination
        // buffer accordingly if possible. Otherwise, throw an exception.
        if (destination.size_ - destination.offset_ < max_copy_byte_count)
        {
            size_type new_size = destination.offset_ + max_copy_byte_count - 2 * destination.size_;
            destination.resize_buffer(new_size);
        }

        // Copy the source buffer section to the destination buffer section
        value_type* destination_buffer = destination.buffer_ + destination.offset_;
        std::copy(destination_buffer, destination_buffer + count, buffer_ + offset_);
    }

    void memory_stream::swap(memory_stream& other)
    {
        std::swap(buffer_, other.buffer_);
        std::swap(size_, other.size_);
        std::swap(owning_buffer_, other.owning_buffer_);

        std::swap(offset_, other.offset_);
    }

    memory_stream& memory_stream::operator=(const memory_stream& other)
    {
        memory_stream{other}.swap(*this);
        return *this;
    }

    memory_stream& memory_stream::operator=(memory_stream&& other) noexcept
    {
        memory_stream{std::move(other)}.swap(*this);
        return *this;
    }

    void memory_stream::resize_buffer(size_type new_size)
    {
        if (!owning_buffer_)
        {
            throw std::runtime_error{"The internal buffer cannot be resized."};
        }

        if (new_size > size_)
        {
            // Create a new temporary buffer and copy the old contents over to the new buffer
            value_type* new_buffer = new value_type[new_size];
            std::copy(new_buffer, new_buffer + size_, buffer_);

            // Cleanup the old buffer since a new buffer exists, and can't throw anymore
            if (buffer_ != nullptr)
            {
                delete[] buffer_;
            }

            // Store reference to the new buffer
            buffer_ = new_buffer;
        }

        size_ = new_size;
    }

    void memory_stream::seek_from_begin(std::int64_t offset)
    {
        if ((offset < 0) || (static_cast<size_type>(offset) > size_))
        {
            throw std::out_of_range{"Seek exceeds range of stream."};
        }

        offset_ = offset;
    }

    void memory_stream::seek_from_current(std::int64_t offset)
    {
        if (((offset < 0) && (offset_ < static_cast<size_type>(-offset))) ||
            ((offset > 0) && (offset_ + offset > size_)))
        {
            throw std::out_of_range{"Seek exceeds range of stream."};
        }

        offset_ += offset;
    }

    void memory_stream::seek_from_end(std::int64_t offset)
    {
        if ((offset < 0) || (static_cast<size_type>(offset) > size_))
        {
            throw std::out_of_range{"Seek exceeds range of stream."};
        }

        offset_ = size_ - offset;
    }
} // namespace streams
