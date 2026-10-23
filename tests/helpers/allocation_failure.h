#ifndef AVIS_TESTS_HELPERS_ALLOCATION_FAILURE_H
#define AVIS_TESTS_HELPERS_ALLOCATION_FAILURE_H

namespace avis::tests
{
    // While alive, makes the next operator new on the calling thread throw std::bad_alloc. Other threads are not
    // affected. Backed by the global operator new replacement in allocation_failure.cpp.
    class scoped_allocation_failure
    {
    public:
        scoped_allocation_failure() noexcept;
        ~scoped_allocation_failure(); // disarms the failure if no allocation used it

        scoped_allocation_failure(const scoped_allocation_failure&) = delete;
        scoped_allocation_failure& operator=(const scoped_allocation_failure&) = delete;

        // True once an allocation has failed because of this scope
        bool triggered() const noexcept;
    };
} // namespace avis::tests

#endif
