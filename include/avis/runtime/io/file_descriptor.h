#ifndef RUNTIME_IO_FILE_DESCRIPTOR_H
#define RUNTIME_IO_FILE_DESCRIPTOR_H

#include "avis/core/common.h"

namespace io
{
    using file_descriptor = std::uintptr_t;

    struct basic_file_descriptor
    {
    private:
        struct file_view
        {
        public:
            file_view() : offset{0}, size{0}, data{} {}
            file_view(std::size_t offset, std::size_t size, unsigned char* data) :
                offset{offset},
                size{size},
                data{data, size}
            {}

        public:
            std::size_t offset;
            std::size_t size;
            std::span<unsigned char> data;
        };

    public:
        basic_file_descriptor() = default;
        explicit basic_file_descriptor(const std::filesystem::path& file_path);
        basic_file_descriptor(const basic_file_descriptor& other) noexcept;
        basic_file_descriptor(basic_file_descriptor&& other) noexcept;
        ~basic_file_descriptor();

        std::span<unsigned char> acquire_view(std::size_t offset, std::size_t size);
        void release_view(const std::span<unsigned char>& view);
        void release_all_views();

        void release();

        std::size_t file_size() const;

        void swap(basic_file_descriptor& other);

        inline const std::filesystem::path& file_path() const;

        inline static basic_file_descriptor* from_file_descriptor(file_descriptor descriptor);
        inline static file_descriptor to_file_descriptor(basic_file_descriptor* descriptor);

        basic_file_descriptor& operator=(const basic_file_descriptor& other) noexcept;
        basic_file_descriptor& operator=(basic_file_descriptor&& other) noexcept;

    private:
        void initialize_handles();

        // Tries to find an active view which encloses the view formed by the given parameters.
        // I.e., if found, this will give a view for which [offset, size) is a subview.
        std::optional<std::span<unsigned char>> try_find_view(std::size_t offset, std::size_t size);

        std::span<unsigned char> create_view(std::size_t offset, std::size_t size);
        void store_view(file_view&& view);

        void check_range_against_file_size(std::size_t file_byte_size, std::size_t offset, std::size_t size);

    private:
        std::filesystem::path file_path_;

        unique_handle file_handle_;
        unique_handle file_mapping_;
        std::once_flag initialization_flag_;

        std::vector<file_view> active_views_;
        std::mutex active_views_mutex_;

        static std::size_t allocation_granularity_;
        static std::once_flag static_initialization_flag_;
    };

    class file_descriptor_manager
    {
    private:
        using descriptor_list = std::unique_ptr<basic_file_descriptor[]>;
        static constexpr std::size_t descriptor_list_size = 32;

    public:
        ENGINE_API file_descriptor_manager();

        ENGINE_API file_descriptor create_descriptor(const std::filesystem::path& file_path);
        ENGINE_API void update_descriptor(const file_descriptor descriptor);
        ENGINE_API void release_descriptor(const file_descriptor descriptor);
        ENGINE_API void release_all_descriptors();

        ENGINE_API file_descriptor find_descriptor(const std::filesystem::path& file_path);

    private:
        descriptor_list allocate_descriptor_list();

        void release_descriptors_in_list(const descriptor_list& list);

        basic_file_descriptor* initialize_descriptor_in_list(
            const std::filesystem::path& file_path, const descriptor_list& list, const std::size_t index) const;
        basic_file_descriptor*
            find_descriptor_in_list(const std::filesystem::path& file_path, const descriptor_list& list) const;

        bool is_managed_descriptor(basic_file_descriptor* descriptor) const;

    private:
        descriptor_list current_descriptor_list_;
        std::size_t next_descriptor_index_;

        std::vector<descriptor_list> used_descriptor_lists_;

        std::mutex current_descriptor_list_mutex_;
        std::mutex used_descriptor_lists_mutex_;
    };

    inline const std::filesystem::path& basic_file_descriptor::file_path() const
    {
        return file_path_;
    }

    basic_file_descriptor* basic_file_descriptor::from_file_descriptor(file_descriptor descriptor)
    {
        return reinterpret_cast<basic_file_descriptor*>(descriptor);
    }

    file_descriptor basic_file_descriptor::to_file_descriptor(basic_file_descriptor* descriptor)
    {
        return reinterpret_cast<file_descriptor>(descriptor);
    }
} // namespace io

#endif
