#ifndef CORE_COM_PTR_H
#define CORE_COM_PTR_H

#include "avis/core/common.h"

namespace details
{
    template<typename com_ptr_t>
    class com_ptr_ref_base
    {
    public:
        using interface_type = typename com_ptr_t::value;

        operator IUnknown**() const;

    protected:
        com_ptr_t* internal_ptr;
    };

    template<typename com_ptr_t>
    class com_ptr_ref : public com_ptr_ref_base<com_ptr_t>
    {
        using base = details::com_ptr_ref_base<com_ptr_t>;
        using interface_type = typename base::interface_type;

    public:
        com_ptr_ref(com_ptr_t* ptr);

        // Conversion operators
        operator void**() const;
        operator com_ptr_t*();

        // Operator interface_type**() is defined here instead of on com_ptr_ref_base<com_ptr_t>, since
        // if interface_type is IUnknown or IInspectable, having it on the base will collide.
        operator interface_type**();

        // This is used for IID_PPV_ARGS in order to do __uuidof(**(ppType)).
        // It does not need to clear internal_ptr at this point, it is done at IID_PPV_ARGS_Helper(com_ptr_ref&) later
        // in this file.
        interface_type* operator*();

        // Explicit functions
        interface_type* const* address_of() const;
    };
} // namespace details

// The implementation of this class was inspired by https://github.com/microsoft/DirectX-Headers/blob/main/include/wsl/wrladapter.h
template<typename ptr_t>
class com_ptr
{
public:
    using value = ptr_t;
    using pointer = value*;

    template<class U>
    friend class ComPtr;

    com_ptr();
    com_ptr(decltype(nullptr));
    template<typename convertible_ptr_t>
    com_ptr(convertible_ptr_t* other);
    template<typename convertible_ptr_t>
    com_ptr(
        const com_ptr<convertible_ptr_t>& other,
        typename std::enable_if<std::is_convertible_v<convertible_ptr_t*, ptr_t*>>::type* = nullptr);
    com_ptr(const com_ptr<ptr_t>& other);
    com_ptr(com_ptr<ptr_t>&& other);
    ~com_ptr();

    com_ptr<ptr_t>& operator=(com_ptr<ptr_t> rhs);

    pointer operator->() const;
    details::com_ptr_ref<com_ptr<ptr_t>> operator&();
    const details::com_ptr_ref<const com_ptr<ptr_t>> operator&() const;

    explicit operator bool() const noexcept;

    pointer get() const;

    pointer* address_of();
    pointer const* address_of() const;

    void release();
    void reset(pointer new_ptr = nullptr);
    void reset(decltype(nullptr));

    template<typename convertible_ptr_t>
    HRESULT as(com_ptr<convertible_ptr_t>& ptr) const;

    void swap(com_ptr<ptr_t>& rhs) noexcept;

private:
    void internal_add_ref() const;
    void internal_release();

    pointer internal_ptr;
};

namespace details
{
    template<typename com_ptr_t>
    com_ptr_ref_base<com_ptr_t>::operator IUnknown**() const
    {
        static_assert(
            std::is_base_of_v<IUnknown, interface_type>, "Invalid cast: interface_type does not derive from IUnknown");

        internal_ptr->release();
        return reinterpret_cast<IUnknown**>(internal_ptr->address_of());
    }

    template<typename com_ptr_t>
    com_ptr_ref<com_ptr_t>::com_ptr_ref(com_ptr_t* ptr) : com_ptr_ref_base<com_ptr_t>()
    {
        com_ptr_ref_base<com_ptr_t>::internal_ptr = ptr;
    }

    template<typename com_ptr_t>
    com_ptr_ref<com_ptr_t>::operator void**() const
    {
        com_ptr_ref_base<com_ptr_t>::internal_ptr->release();
        return reinterpret_cast<void**>(com_ptr_ref_base<com_ptr_t>::internal_ptr->address_of());
    }

    template<typename com_ptr_t>
    com_ptr_ref<com_ptr_t>::operator com_ptr_t*()
    {
        *com_ptr_ref_base<com_ptr_t>::internal_ptr = nullptr;
        return com_ptr_ref_base<com_ptr_t>::internal_ptr;
    }

    template<typename com_ptr_t>
    com_ptr_ref<com_ptr_t>::operator interface_type**()
    {
        com_ptr_ref_base<com_ptr_t>::internal_ptr->release();
        return com_ptr_ref_base<com_ptr_t>::internal_ptr->address_of();
    }

    template<typename com_ptr_t>
    typename com_ptr_ref<com_ptr_t>::interface_type* com_ptr_ref<com_ptr_t>::operator*()
    {
        return com_ptr_ref_base<com_ptr_t>::internal_ptr->get();
    }

    template<typename com_ptr_t>
    typename com_ptr_ref<com_ptr_t>::interface_type* const* com_ptr_ref<com_ptr_t>::address_of() const
    {
        return com_ptr_ref_base<com_ptr_t>::internal_ptr->address_of();
    }
} // namespace details

template<typename ptr_t>
com_ptr<ptr_t>::com_ptr() : internal_ptr{ nullptr }
{}

template<typename ptr_t>
com_ptr<ptr_t>::com_ptr(decltype(nullptr)) : internal_ptr{ nullptr }
{}

template<typename ptr_t>
template<typename U>
com_ptr<ptr_t>::com_ptr(U* other) : internal_ptr{ other }
{
    internal_add_ref();
}

template<typename ptr_t>
template<typename convertible_ptr_t>
com_ptr<ptr_t>::com_ptr(
    const com_ptr<convertible_ptr_t>& other,
    typename std::enable_if<std::is_convertible_v<convertible_ptr_t*, ptr_t*>>::type*) :
    internal_ptr{ other.internal_ptr }
{
    internal_add_ref();
}

template<typename ptr_t>
com_ptr<ptr_t>::com_ptr(const com_ptr<ptr_t>& other) : internal_ptr{ other.internal_ptr }
{
    internal_add_ref();
}

template<typename ptr_t>
com_ptr<ptr_t>::com_ptr(com_ptr<ptr_t>&& other) : internal_ptr{ other.internal_ptr }
{
    other.internal_ptr = nullptr;
}

template<typename ptr_t>
com_ptr<ptr_t>::~com_ptr()
{
    internal_release();
}

template<typename ptr_t>
com_ptr<ptr_t>& com_ptr<ptr_t>::operator=(com_ptr<ptr_t> rhs)
{
    swap(rhs);
    return *this;
}

template<typename ptr_t>
com_ptr<ptr_t>::pointer com_ptr<ptr_t>::operator->() const
{
    return internal_ptr;
}

template<typename ptr_t>
details::com_ptr_ref<com_ptr<ptr_t>> com_ptr<ptr_t>::operator&()
{
    return details::com_ptr_ref<com_ptr<ptr_t>>(this);
}

template<typename ptr_t>
const details::com_ptr_ref<const com_ptr<ptr_t>> com_ptr<ptr_t>::operator&() const
{
    return details::com_ptr_ref<const com_ptr<ptr_t>>(this);
}

template<typename ptr_t>
com_ptr<ptr_t>::operator bool() const noexcept
{
    return internal_ptr != nullptr;
}

template<typename ptr_t>
com_ptr<ptr_t>::pointer com_ptr<ptr_t>::get() const
{
    return internal_ptr;
}

template<typename ptr_t>
typename com_ptr<ptr_t>::pointer* com_ptr<ptr_t>::address_of()
{
    return &internal_ptr;
}

template<typename ptr_t>
typename com_ptr<ptr_t>::pointer const* com_ptr<ptr_t>::address_of() const
{
    return &internal_ptr;
}

template<typename ptr_t>
void com_ptr<ptr_t>::release()
{
    internal_release();
}

template<typename ptr_t>
void com_ptr<ptr_t>::reset(pointer new_ptr)
{
    internal_release();

    internal_ptr = new_ptr;
    internal_add_ref();
}

template<typename ptr_t>
void com_ptr<ptr_t>::reset(decltype(nullptr))
{
    internal_release();
}

template<typename ptr_t>
template<typename convertible_ptr_t>
HRESULT com_ptr<ptr_t>::as(com_ptr<convertible_ptr_t>& other) const
{
    // Release any pointer that was held by other so it can be overwritten by the new pointer
    other.release();

    return internal_ptr->QueryInterface(__uuidof(convertible_ptr_t), reinterpret_cast<void**>(other.address_of()));
}

template<typename ptr_t>
void com_ptr<ptr_t>::swap(com_ptr<ptr_t>& rhs) noexcept
{
    using std::swap;
    swap(internal_ptr, rhs.internal_ptr);
}

template<typename ptr_t>
void com_ptr<ptr_t>::internal_add_ref() const
{
    if (internal_ptr != nullptr)
    {
        internal_ptr->AddRef();
    }
}

template<typename ptr_t>
void com_ptr<ptr_t>::internal_release()
{
    if (internal_ptr != nullptr)
    {
        internal_ptr->Release();
        internal_ptr = nullptr;
    }
}

template<typename ptr_t>
void swap(com_ptr<ptr_t>& lhs, com_ptr<ptr_t>& rhs) noexcept
{
    lhs.swap(rhs);
}

// Overloaded global function to provide to IID_PPV_ARGS that support details::com_ptr_ref
template<typename T>
void** IID_PPV_ARGS_Helper(details::com_ptr_ref<T> ptr) throw()
{
    return ptr;
}

#endif
