#include "avis/graphics/linear_arena.h"

#include "avis/detail/check.h"

#include <bit>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>

namespace avis::graphics
{
    namespace
    {
        constexpr std::string_view logger_name = "graphics";

        constexpr std::uintptr_t align_up(const std::uintptr_t value, const std::size_t alignment) noexcept
        {
            return (value + alignment - 1) & ~static_cast<std::uintptr_t>(alignment - 1);
        }
    } // namespace

    // A block's header, followed by its data; one OS allocation. The alignment keeps the data max_align_t-aligned
    // (operator new returns at least that).
    struct alignas(std::max_align_t) linear_arena::block
    {
        block* next = nullptr;
        std::size_t capacity = 0; // usable bytes after the header
        std::size_t allocation_size = 0; // bytes from the OS, header included

        std::byte* data() noexcept
        {
            return reinterpret_cast<std::byte*>(this + 1);
        }

        static block* create(const std::size_t capacity)
        {
            if (capacity > std::numeric_limits<std::size_t>::max() - sizeof(block))
            {
                throw std::bad_alloc{};
            }
            const std::size_t allocation_size = sizeof(block) + capacity;
            void* const memory = ::operator new(allocation_size);
            return ::new (memory) block{ nullptr, capacity, allocation_size };
        }

        static void destroy(block* const instance) noexcept
        {
            instance->~block();
            ::operator delete(static_cast<void*>(instance));
        }
    };

    // Allocated in the arena itself; newest first, so reset() walks them in reverse creation order
    struct linear_arena::destructor_record
    {
        destructor_record* previous = nullptr;
        void* object = nullptr;
        void (*destroy)(void*) noexcept = nullptr;
    };

    linear_arena::linear_arena(const linear_arena_configuration& configuration) : configuration_{ configuration }
    {
        AVIS_CHECK(logger_name, configuration_.block_size > 0, "linear_arena: block_size must not be 0");
        if (configuration_.block_size == 0)
        {
            configuration_.block_size = linear_arena_configuration{}.block_size;
        }
    }

    linear_arena::~linear_arena()
    {
        run_destructors();
        free_dedicated_blocks();
        for (block* current = first_block_; current != nullptr;)
        {
            block* const next = current->next;
            block::destroy(current);
            current = next;
        }
    }

    void* linear_arena::allocate(const std::size_t size, std::size_t alignment)
    {
        if (!AVIS_CHECK(
                logger_name, std::has_single_bit(alignment), "linear_arena::allocate: alignment isn't a power of two"))
        {
            alignment = std::bit_ceil(alignment); // 0 becomes 1
        }

        // Worst-case padding counts too, so a request that fits the dedicated test always fits an empty block
        const std::size_t block_size = configuration_.block_size;
        if (size > block_size / 2 || alignment - 1 > block_size - size)
        {
            return allocate_dedicated(size, alignment);
        }

        for (;;)
        {
            if (current_block_ != nullptr)
            {
                const auto base = reinterpret_cast<std::uintptr_t>(current_block_->data());
                const std::uintptr_t aligned = align_up(base + current_offset_, alignment);
                const std::size_t end = static_cast<std::size_t>(aligned - base) + size;
                if (end <= current_block_->capacity)
                {
                    current_offset_ = end;
                    return current_block_->data() + (aligned - base);
                }
            }
            advance_block();
        }
    }

    void linear_arena::reset() noexcept
    {
        run_destructors();
        free_dedicated_blocks();

        // Keep the first maximum_retained_blocks regular blocks, release the rest
        block* last_retained = nullptr;
        block* current = first_block_;
        std::size_t retained = 0;
        while (current != nullptr && retained < configuration_.maximum_retained_blocks)
        {
            last_retained = current;
            current = current->next;
            ++retained;
        }
        while (current != nullptr)
        {
            block* const next = current->next;
            reserved_ -= current->allocation_size;
            block::destroy(current);
            current = next;
        }
        if (last_retained != nullptr)
        {
            last_retained->next = nullptr;
        }
        else
        {
            first_block_ = nullptr;
        }

        current_block_ = first_block_;
        current_offset_ = 0;
        used_in_earlier_blocks_ = 0;
        used_in_dedicated_blocks_ = 0;
    }

    std::size_t linear_arena::bytes_used() const noexcept
    {
        return used_in_earlier_blocks_ + current_offset_ + used_in_dedicated_blocks_;
    }

    std::size_t linear_arena::bytes_reserved() const noexcept
    {
        return reserved_;
    }

    linear_arena::destructor_record* linear_arena::reserve_destructor_record()
    {
        return static_cast<destructor_record*>(allocate(sizeof(destructor_record), alignof(destructor_record)));
    }

    void linear_arena::push_destructor(
        destructor_record* const record, void* const object, void (*const destroy)(void*) noexcept) noexcept
    {
        last_destructor_ = ::new (record) destructor_record{ last_destructor_, object, destroy };
    }

    void* linear_arena::allocate_dedicated(const std::size_t size, const std::size_t alignment)
    {
        if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1))
        {
            throw std::bad_alloc{};
        }
        block* const dedicated = block::create(size + alignment - 1);
        dedicated->next = dedicated_blocks_;
        dedicated_blocks_ = dedicated;
        reserved_ += dedicated->allocation_size;

        const auto base = reinterpret_cast<std::uintptr_t>(dedicated->data());
        const std::uintptr_t aligned = align_up(base, alignment);
        used_in_dedicated_blocks_ += static_cast<std::size_t>(aligned - base) + size;
        return dedicated->data() + (aligned - base);
    }

    void linear_arena::advance_block()
    {
        if (current_block_ == nullptr)
        {
            // Nothing allocated yet, or reset() released every block
            first_block_ = block::create(configuration_.block_size);
            reserved_ += first_block_->allocation_size;
            current_block_ = first_block_;
            current_offset_ = 0;
            return;
        }

        if (current_block_->next == nullptr)
        {
            block* const next = block::create(configuration_.block_size);
            reserved_ += next->allocation_size;
            current_block_->next = next;
        }
        used_in_earlier_blocks_ += current_offset_;
        current_block_ = current_block_->next;
        current_offset_ = 0;
    }

    void linear_arena::run_destructors() noexcept
    {
        destructor_record* record = last_destructor_;
        last_destructor_ = nullptr;
        while (record != nullptr)
        {
            destructor_record* const previous = record->previous;
            record->destroy(record->object);
            record = previous;
        }
    }

    void linear_arena::free_dedicated_blocks() noexcept
    {
        for (block* current = dedicated_blocks_; current != nullptr;)
        {
            block* const next = current->next;
            reserved_ -= current->allocation_size;
            block::destroy(current);
            current = next;
        }
        dedicated_blocks_ = nullptr;
    }
} // namespace avis::graphics
