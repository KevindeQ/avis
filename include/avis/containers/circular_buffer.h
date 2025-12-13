#ifndef CONTAINERS_CIRCULAR_BUFFER_H
#define CONTAINERS_CIRCULAR_BUFFER_H

#include "avis/common.h"

namespace containers
{
    template<typename value_type_t>
    class circular_buffer_iterator
    {};

    namespace details
    {
        template<typename value_type_t, size_t extent>
        class circular_buffer_storage
        {
        public:
        private:
            std::array<value_type_t, extent> buffer;
        };

        template<typename value_type_t>
        class circular_buffer_storage<value_type_t, std::dynamic_extent>
        {
        public:
        private:
            std::vector<value_type_t> buffer;
        };
    } // namespace details

    template<typename value_type_t, size_t extent = std::dynamic_extent>
    class circular_buffer : private details::circular_buffer_storage<value_type_t, extent>
    {
    public:
        using value_type = std::remove_cv_t<value_type_t>;
        using reference = value_type&;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using const_reference = const value_type&;
        using pointer = value_type*;
        using const_pointer = const value_type*;
        using iterator = circular_buffer_iterator<value_type>;
        using const_iterator = std::reverse_iterator<iterator>;

        circular_buffer();

        template<std::enable_if_t<extent == std::dynamic_extent, bool> = true>
        circular_buffer(const size_type size);

        circular_buffer(const circular_buffer& other);
        circular_buffer(circular_buffer&& other);

        circular_buffer& operator=(const circular_buffer& other);
        circular_buffer& operator=(circular_buffer&& other);

        reference operator[](const size_type index);
        const_reference operator[](const size_type index) const;

        void push_back(const value_type& data);
        void push_back(value_type&& data) noexcept;

        void pop_front();

        void clear();

        reference front();
        reference back();
        const_reference front() const;
        const_reference back() const;

        reference at(const size_type index);
        const_reference at(const size_type index) const;

        pointer data() const;

        iterator begin();
        iterator end();
        const_iterator begin() const;
        const_iterator end() const;
        const_iterator cbegin() const noexcept;
        const_iterator cend() const noexcept;
        iterator rbegin() noexcept;
        iterator rend() noexcept;
        const_iterator rbegin() const noexcept;
        const_iterator rend() const noexcept;

        bool empty() const;

        size_type size() const;
        size_type capacity() const;
    };

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::circular_buffer()
    {}

    template<typename value_type_t, size_t extent>
    template<std::enable_if_t<extent == std::dynamic_extent, bool>>
    circular_buffer<value_type_t, extent>::circular_buffer(const size_type size)
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::circular_buffer(const circular_buffer& other)
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::circular_buffer(circular_buffer&& other)
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>&
        circular_buffer<value_type_t, extent>::operator=(const circular_buffer& other)
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>& circular_buffer<value_type_t, extent>::operator=(circular_buffer&& other)
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::reference
        circular_buffer<value_type_t, extent>::operator[](const size_type index)
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::const_reference
        circular_buffer<value_type_t, extent>::operator[](const size_type index) const
    {}

    template<typename value_type_t, size_t extent>
    void circular_buffer<value_type_t, extent>::push_back(const value_type& data)
    {}

    template<typename value_type_t, size_t extent>
    void circular_buffer<value_type_t, extent>::push_back(value_type&& data) noexcept
    {}

    template<typename value_type_t, size_t extent>
    void circular_buffer<value_type_t, extent>::pop_front()
    {}

    template<typename value_type_t, size_t extent>
    void circular_buffer<value_type_t, extent>::clear()
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::reference circular_buffer<value_type_t, extent>::front()
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::reference circular_buffer<value_type_t, extent>::back()
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::const_reference circular_buffer<value_type_t, extent>::front() const
    {}
    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::const_reference circular_buffer<value_type_t, extent>::back() const
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::reference circular_buffer<value_type_t, extent>::at(const size_type index)
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::const_reference
        circular_buffer<value_type_t, extent>::at(const size_type index) const
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::pointer circular_buffer<value_type_t, extent>::data() const
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::iterator circular_buffer<value_type_t, extent>::begin()
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::iterator circular_buffer<value_type_t, extent>::end()
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::const_iterator circular_buffer<value_type_t, extent>::begin() const
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::const_iterator circular_buffer<value_type_t, extent>::end() const
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::const_iterator circular_buffer<value_type_t, extent>::cbegin() const noexcept
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::const_iterator circular_buffer<value_type_t, extent>::cend() const noexcept
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::iterator circular_buffer<value_type_t, extent>::rbegin() noexcept
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::iterator circular_buffer<value_type_t, extent>::rend() noexcept
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::const_iterator circular_buffer<value_type_t, extent>::rbegin() const noexcept
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::const_iterator circular_buffer<value_type_t, extent>::rend() const noexcept
    {}

    template<typename value_type_t, size_t extent>
    bool circular_buffer<value_type_t, extent>::empty() const
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::size_type circular_buffer<value_type_t, extent>::size() const
    {}

    template<typename value_type_t, size_t extent>
    circular_buffer<value_type_t, extent>::size_type circular_buffer<value_type_t, extent>::capacity() const
    {}
} // namespace containers

#endif
