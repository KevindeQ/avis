#include "allocation_failure.h"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{
    // Constant-initialised, so reading them from operator new needs no dynamic TLS initialisation
    thread_local bool fail_next_allocation = false;
    thread_local bool allocation_failed = false;

    void* allocate(std::size_t size)
    {
        if (fail_next_allocation)
        {
            fail_next_allocation = false;
            allocation_failed = true;
            throw std::bad_alloc{};
        }

        if (size == 0)
        {
            size = 1;
        }

        while (true)
        {
            if (void* memory = std::malloc(size))
            {
                return memory;
            }

            const std::new_handler handler = std::get_new_handler();
            if (!handler)
            {
                throw std::bad_alloc{};
            }
            handler();
        }
    }
} // namespace

namespace avis::tests
{
    scoped_allocation_failure::scoped_allocation_failure() noexcept
    {
        fail_next_allocation = true;
        allocation_failed = false;
    }

    scoped_allocation_failure::~scoped_allocation_failure()
    {
        fail_next_allocation = false;
    }

    bool scoped_allocation_failure::triggered() const noexcept
    {
        return allocation_failed;
    }
} // namespace avis::tests

// Replacements for the whole test executable. The aligned and nothrow forms keep their defaults: the nothrow ones
// call these, and the aligned ones use their own allocator consistently.
void* operator new(const std::size_t size)
{
    return allocate(size);
}

void* operator new[](const std::size_t size)
{
    return allocate(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}
