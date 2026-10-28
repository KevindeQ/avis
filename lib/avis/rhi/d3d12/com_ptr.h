#ifndef AVIS_RHI_COM_PTR_H
#define AVIS_RHI_COM_PTR_H

// The RHI's single include point for the Windows headers (with NOMINMAX), instead of <Unknwn.h> directly
#include "avis/rhi/d3d12/d3d12.h"

#include <type_traits>
#include <utility>

namespace avis::rhi
{
    namespace details
    {
        template<typename com_ptr_t>
        class com_ptr_ref_base
        {
        public:
            using interface_type = typename com_ptr_t::value;

            operator IUnknown**() const
            {
                static_assert(
                    std::is_base_of_v<IUnknown, interface_type>,
                    "Invalid cast: interface_type does not derive from IUnknown");

                internal_ptr_->release();
                return reinterpret_cast<IUnknown**>(internal_ptr_->address_of());
            }

        protected:
            com_ptr_t* internal_ptr_;
        };

        template<typename com_ptr_t>
        class com_ptr_ref : public com_ptr_ref_base<com_ptr_t>
        {
            using base = details::com_ptr_ref_base<com_ptr_t>;
            using interface_type = typename base::interface_type;

        public:
            com_ptr_ref(com_ptr_t* ptr) : com_ptr_ref_base<com_ptr_t>()
            {
                com_ptr_ref_base<com_ptr_t>::internal_ptr_ = ptr;
            }

            // Conversion operators
            operator void**() const
            {
                com_ptr_ref_base<com_ptr_t>::internal_ptr_->release();
                return reinterpret_cast<void**>(com_ptr_ref_base<com_ptr_t>::internal_ptr_->address_of());
            }

            operator com_ptr_t*()
            {
                *com_ptr_ref_base<com_ptr_t>::internal_ptr_ = nullptr;
                return com_ptr_ref_base<com_ptr_t>::internal_ptr_;
            }

            // Operator interface_type**() is defined here instead of on com_ptr_ref_base<com_ptr_t>, since
            // if interface_type is IUnknown or IInspectable, having it on the base will collide.
            operator interface_type**()
            {
                com_ptr_ref_base<com_ptr_t>::internal_ptr_->release();
                return com_ptr_ref_base<com_ptr_t>::internal_ptr_->address_of();
            }

            // This is used for IID_PPV_ARGS in order to do __uuidof(**(ppType)).
            // It does not need to clear internal_ptr_ at this point, it is done at IID_PPV_ARGS_Helper(com_ptr_ref&)
            // later in this file.
            interface_type* operator*()
            {
                return com_ptr_ref_base<com_ptr_t>::internal_ptr_->get();
            }

            // Explicit functions
            interface_type* const* address_of() const
            {
                return com_ptr_ref_base<com_ptr_t>::internal_ptr_->address_of();
            }
        };
    } // namespace details

    // The implementation of this class was inspired by
    // https://github.com/microsoft/DirectX-Headers/blob/main/include/wsl/wrladapter.h
    template<typename ptr_t>
    class com_ptr
    {
    public:
        using value = ptr_t;
        using pointer = value*;

        template<class U>
        friend class ComPtr;

        com_ptr() : internal_ptr_{ nullptr } {}

        com_ptr(decltype(nullptr)) : internal_ptr_{ nullptr } {}

        template<typename convertible_ptr_t>
        com_ptr(convertible_ptr_t* other) : internal_ptr_{ other }
        {
            internal_add_ref();
        }

        template<typename convertible_ptr_t>
        com_ptr(
            const com_ptr<convertible_ptr_t>& other,
            typename std::enable_if<std::is_convertible_v<convertible_ptr_t*, ptr_t*>>::type* = nullptr) :
            internal_ptr_{ other.internal_ptr_ }
        {
            internal_add_ref();
        }

        com_ptr(const com_ptr<ptr_t>& other) : internal_ptr_{ other.internal_ptr_ }
        {
            internal_add_ref();
        }

        com_ptr(com_ptr<ptr_t>&& other) : internal_ptr_{ other.internal_ptr_ }
        {
            other.internal_ptr_ = nullptr;
        }

        ~com_ptr()
        {
            internal_release();
        }

        com_ptr<ptr_t>& operator=(com_ptr<ptr_t> rhs)
        {
            swap(rhs);
            return *this;
        }

        pointer operator->() const
        {
            return internal_ptr_;
        }

        details::com_ptr_ref<com_ptr<ptr_t>> operator&()
        {
            return details::com_ptr_ref<com_ptr<ptr_t>>(this);
        }

        const details::com_ptr_ref<const com_ptr<ptr_t>> operator&() const
        {
            return details::com_ptr_ref<const com_ptr<ptr_t>>(this);
        }

        explicit operator bool() const noexcept
        {
            return internal_ptr_ != nullptr;
        }

        pointer get() const
        {
            return internal_ptr_;
        }

        pointer* address_of()
        {
            return &internal_ptr_;
        }

        pointer const* address_of() const
        {
            return &internal_ptr_;
        }

        void release()
        {
            internal_release();
        }

        void reset(pointer new_ptr = nullptr)
        {
            internal_release();

            internal_ptr_ = new_ptr;
            internal_add_ref();
        }

        void reset(decltype(nullptr))
        {
            internal_release();
        }

        template<typename convertible_ptr_t>
        HRESULT as(com_ptr<convertible_ptr_t>& other) const
        {
            // Release any pointer that was held by other so it can be overwritten by the new pointer
            other.release();

            return internal_ptr_->QueryInterface(
                __uuidof(convertible_ptr_t), reinterpret_cast<void**>(other.address_of()));
        }

        void swap(com_ptr<ptr_t>& rhs) noexcept
        {
            using std::swap;
            swap(internal_ptr_, rhs.internal_ptr_);
        }

    protected:
        template<class U>
        friend class com_ptr;

    private:
        void internal_add_ref() const
        {
            if (internal_ptr_ != nullptr)
            {
                internal_ptr_->AddRef();
            }
        }

        void internal_release()
        {
            if (internal_ptr_ != nullptr)
            {
                internal_ptr_->Release();
                internal_ptr_ = nullptr;
            }
        }

        pointer internal_ptr_;
    };
} // namespace avis::rhi

template<typename ptr_t>
void swap(avis::rhi::com_ptr<ptr_t>& lhs, avis::rhi::com_ptr<ptr_t>& rhs) noexcept
{
    lhs.swap(rhs);
}

// Overloaded global function to provide to IID_PPV_ARGS that support details::com_ptr_ref
template<typename T>
void** IID_PPV_ARGS_Helper(avis::rhi::details::com_ptr_ref<T> ptr) throw()
{
    return ptr;
}

#endif
