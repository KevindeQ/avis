#ifndef RUNTIME_DATA_PAGE_CONTROLLER_H
#define RUNTIME_DATA_PAGE_CONTROLLER_H

#include "avis/core/common.h"
#include "avis/runtime/data/page_view.h"
#include "avis/runtime/io/file_descriptor.h"

namespace data
{
    class page_controller
    {
    public:
        page_controller(io::basic_file_descriptor& file);
        page_controller(io::basic_file_descriptor& file, const std::size_t page_size_in_bytes);

        page_view fetch_page(const std::size_t page_index);
        page_view allocate_page();
        void clear_page(page_view& page);

        std::uint64_t page_count() const;

    private:
        void load_header();
        void initialize_empty_file();

        page_view internal_allocate_page(const std::size_t offset, const std::size_t size);

        io::basic_file_descriptor& descriptor;
        std::size_t page_size;
        page_view header_page;

        static constexpr std::size_t page_offset_magic_number = 0;
        static constexpr std::size_t page_offset_file_version = 6;
        static constexpr std::size_t page_offset_page_size = 8;
        static constexpr std::size_t page_offset_page_count = 16;
        static constexpr std::size_t page_offset_page_id_last_empty = 24;

        static constexpr std::size_t page_offset_page_usage = 0;
        static constexpr std::size_t page_offset_page_id_next = 4;

        static constexpr std::size_t page_offset_data_start = 4;

        static constexpr std::uint64_t magic_number = 0x4156495344420000;
        static constexpr std::uint64_t magic_number_mask = 0xFFFFFFFFFFFF0000;

        static constexpr std::uint16_t database_version = 0x0001;

        static constexpr std::size_t page_size_default = 8192;

        static constexpr std::uint32_t mark_page_used = 0xFFFFFFFF;

        // TODO: Cache the loaded pages.
        // TODO: Separate loading of memory views from file_descriptor.
        /*lru_cache<page_view> page_table;*/
    };
} // namespace data

#endif
