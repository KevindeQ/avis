#ifndef AVIS_GRAPHICS_LINEAR_ARENA_H
#define AVIS_GRAPHICS_LINEAR_ARENA_H

// Per-frame bump allocator for the render graph (docs/design/10-render-graph.md §3.5, D-30): pass nodes, pass data,
// execute callables, resource tables and compiler scratch data, all released together by reset(). Not thread-safe.

#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace avis::graphics
{
    struct linear_arena_configuration
    {
        std::size_t block_size = 256 * 1024; // usable bytes per block
        std::size_t maximum_retained_blocks = 64; // blocks kept across resets (16 MiB with the default size)
    };

    class linear_arena
    {
    public:
        explicit linear_arena(const linear_arena_configuration& configuration = {});
        // Runs pending destructors, then frees all blocks
        ~linear_arena();

        linear_arena(const linear_arena&) = delete;
        linear_arena& operator=(const linear_arena&) = delete;

        // Never returns nullptr; throws std::bad_alloc only when the OS is out of memory. `alignment` must be a power
        // of two (debug check; release builds round it up). Requests larger than half a block get a dedicated block,
        // which reset() frees.
        void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

        // Constructs T in the arena. Registers ~T for reset() unless T is trivially destructible.
        template<typename T, typename... Arguments>
        T& create(Arguments&&... arguments)
        {
            void* const memory = allocate(sizeof(T), alignof(T));
            if constexpr (std::is_trivially_destructible_v<T>)
            {
                return *::new (memory) T(std::forward<Arguments>(arguments)...);
            }
            else
            {
                // The record is reserved before construction, so nothing can fail between constructing T and
                // registering its destructor
                destructor_record* const record = reserve_destructor_record();
                T* const object = ::new (memory) T(std::forward<Arguments>(arguments)...);
                push_destructor(record, object, &destroy<T>);
                return *object;
            }
        }

        // Uninitialised array of trivially-copyable T
        template<typename T>
        std::span<T> allocate_array(const std::size_t count)
        {
            static_assert(std::is_trivially_copyable_v<T>, "allocate_array needs a trivially copyable type");
            if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            {
                throw std::bad_array_new_length{};
            }
            return { static_cast<T*>(allocate(count * sizeof(T), alignof(T))), count };
        }

        // Runs registered destructors in reverse creation order, frees dedicated blocks, rewinds to the first block
        // and releases blocks beyond maximum_retained_blocks
        void reset() noexcept;

        // Bytes handed out since the last reset, alignment padding and destructor records included (debug overlay)
        std::size_t bytes_used() const noexcept;
        // Bytes currently held from the OS, block headers included
        std::size_t bytes_reserved() const noexcept;

    private:
        struct block;
        struct destructor_record;

        template<typename T>
        static void destroy(void* const object) noexcept
        {
            static_cast<T*>(object)->~T();
        }

        destructor_record* reserve_destructor_record();
        void push_destructor(destructor_record* record, void* object, void (*destroy)(void*) noexcept) noexcept;

        void* allocate_dedicated(std::size_t size, std::size_t alignment);
        void advance_block();
        void run_destructors() noexcept;
        void free_dedicated_blocks() noexcept;

        linear_arena_configuration configuration_;

        block* first_block_ = nullptr; // regular blocks, a singly linked list in allocation order
        block* current_block_ = nullptr;
        std::size_t current_offset_ = 0; // bytes used in the current block

        block* dedicated_blocks_ = nullptr; // freed by reset()

        destructor_record* last_destructor_ = nullptr; // newest first

        std::size_t used_in_earlier_blocks_ = 0;
        std::size_t used_in_dedicated_blocks_ = 0;
        std::size_t reserved_ = 0;
    };

    // std-compatible allocator over a linear_arena; deallocate is a no-op, memory returns on reset()
    template<typename T>
    class arena_allocator
    {
    public:
        using value_type = T;

        explicit arena_allocator(linear_arena& arena) noexcept : arena_{ &arena } {}

        template<typename U>
        arena_allocator(const arena_allocator<U>& other) noexcept : arena_{ &other.arena() }
        {}

        T* allocate(const std::size_t count)
        {
            if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            {
                throw std::bad_array_new_length{};
            }
            return static_cast<T*>(arena_->allocate(count * sizeof(T), alignof(T)));
        }

        void deallocate(T*, std::size_t) noexcept {}

        linear_arena& arena() const noexcept
        {
            return *arena_;
        }

        // Allocators of one arena are interchangeable
        template<typename U>
        bool operator==(const arena_allocator<U>& other) const noexcept
        {
            return arena_ == &other.arena();
        }

    private:
        linear_arena* arena_;
    };
} // namespace avis::graphics

#endif
