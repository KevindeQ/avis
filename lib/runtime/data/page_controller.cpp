#include "avis/runtime/data/page_controller.h"

namespace data
{
    page_controller::page_controller(io::basic_file_descriptor& file) :
        descriptor{ file },
        page_size{ page_size_default },
        header_page{}
    {
        load_header();
    }

    page_controller::page_controller(io::basic_file_descriptor& file, const std::size_t page_size_in_bytes) :
        descriptor{ file },
        page_size{ page_size_in_bytes },
        header_page{}
    {
        initialize_empty_file();
    }

    page_view page_controller::fetch_page(const std::size_t page_index)
    {
        std::size_t page_offset = page_index * page_size;
        page_view requested_page = internal_allocate_page(page_offset, page_size);

        // Check if the requested page is actually in-use
        std::optional<std::uint32_t> page_used = requested_page.read_uint32_le(page_offset_page_usage);
        if (!page_used.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Unable to determine page status." };
        }
        if (page_used.value() != mark_page_used)
        {
            throw std::runtime_error{ "Requested an unused page" };
        }

        return requested_page;
    }

    page_view page_controller::allocate_page()
    {
        // Check the free list for an available empty page
        std::optional<std::uint64_t> page_id_last_empty = header_page.read_uint64_le(page_offset_page_id_last_empty);
        if (!page_id_last_empty.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Could not read page free list." };
        }

        if (page_id_last_empty.value() != 0)
        {
            // Load the last empty page
            page_view last_page_empty = internal_allocate_page((page_id_last_empty.value() + 1) * page_size, page_size);

            // Get the index of the first empty page
            std::optional<std::uint64_t> page_id_first_page_empty =
                last_page_empty.read_uint64_le(page_offset_page_id_next);
            if (!page_id_first_page_empty.has_value())
            {
                throw std::runtime_error{ "Invalid file format. Could not read first empty page id." };
            }

            // Load the first empty page
            page_view first_page_empty =
                internal_allocate_page((page_id_first_page_empty.value() + 1) * page_size, page_size);

            // Get the index of the second empty page
            std::optional<std::uint64_t> page_id_second_page_empty =
                first_page_empty.read_uint64_le(page_offset_page_id_next);
            if (!page_id_second_page_empty.has_value())
            {
                throw std::runtime_error{ "Invalid file format. Could not read next empty page id." };
            }

            if (page_id_last_empty.value() == page_id_second_page_empty.value())
            {
                // A self-reference was found meaning there is no empty page after this removal process.
                // So, remove reference to empty pages from the header.
                header_page.write_uint64_le(0, page_offset_page_id_last_empty);
            }
            else
            {
                // Remove the first page from the free list by letting the last page point to the second page
                last_page_empty.write_uint64_le(page_id_second_page_empty.value(), page_offset_page_id_next);
            }

            // Prepare first empty page for usage
            first_page_empty.clear();

            // Mark the page as being in-use
            first_page_empty.write_uint32_le(mark_page_used, page_offset_page_usage);

            return first_page_empty;
        }
        else
        {
            std::optional<std::uint64_t> page_count = header_page.read_uint64_le(page_offset_page_count);
            if (!page_count.has_value())
            {
                throw std::runtime_error{ "Invalid file format. Unable to read page count." };
            }

            std::size_t new_page_offset = (page_count.value() + 1) * page_size;
            page_view new_page = internal_allocate_page(new_page_offset, page_size);

            // Reset the bytes on the entire page to a known value
            new_page.clear();

            // Mark a page as being in-use
            new_page.write_uint32_le(mark_page_used, page_offset_page_usage);

            // Update the number of currently used pages
            header_page.write_uint64_le(page_count.value() + 1, page_offset_page_count);

            return new_page;
        }
    }

    void page_controller::clear_page(page_view& page)
    {
        // Clear the page from usage and prepare for storage in the free list
        page.clear();

        // Load the last empty page in the free list so the cleared page can be included
        std::optional<std::uint64_t> page_id_last_empty = header_page.read_uint64_le(page_offset_page_id_last_empty);
        if (!page_id_last_empty.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Could not read page free list." };
        }

        // The header page is not included in the page ids. So, offset the pages by the header page.
        std::size_t page_offset = (page_id_last_empty.value() + 1) * page_size;
        page_view last_empty_page = internal_allocate_page(page_offset, page_size);

        std::optional<std::uint64_t> next_page_id = last_empty_page.read_uint64_le(page_offset_page_id_next);
        if (!next_page_id.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Could not read page id in free list" };
        }

        // Copy next page id from the last empty page
        page.write_uint64_le(next_page_id.value(), page_offset_page_id_next);

        // Write page id of the new empty page to the end of the free list and remember it in the header
        std::uint64_t new_page_id = page.id();
        last_empty_page.write_uint64_le(new_page_id, page_offset_page_id_next);
        header_page.write_uint64_le(new_page_id, page_offset_page_id_last_empty);
    }

    std::uint64_t page_controller::page_count() const
    {
        std::optional<std::uint64_t> page_count = header_page.read_uint64_le(page_offset_page_count);
        if (!page_count.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Unable to read page count." };
        }

        return page_count.value();
    }

    void page_controller::load_header()
    {
        header_page = internal_allocate_page(0, page_size);

        // Read and check the magic number
        std::optional<std::uint64_t> file_magic_number = header_page.read_uint64_le(page_offset_magic_number);
        if (!file_magic_number.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Unable to read the magic number." };
        }
        if ((file_magic_number.value() & magic_number_mask) != magic_number)
        {
            throw std::runtime_error{ "Unrecognized magic number" };
        }

        // Read the file version
        std::optional<std::uint16_t> file_database_version = header_page.read_uint16_le(page_offset_file_version);
        if (!file_database_version.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Unable to read the database version." };
        }
        if (file_database_version.value() != database_version)
        {
            throw std::runtime_error{ "Unsupported database version" };
        }

        // Read the page size
        std::optional<std::uint64_t> file_page_size = header_page.read_uint64_le(page_offset_page_size);
        if (!file_page_size.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Unable to read page size." };
        }
        page_size = file_page_size.value();
    }

    void page_controller::initialize_empty_file()
    {
        header_page = internal_allocate_page(0, page_size);
        page_view data_page = internal_allocate_page(page_size, page_size);

        // Write header in the first page of the file
        header_page.write_uint64_le(magic_number, page_offset_magic_number);
        header_page.write_uint16_le(database_version, page_offset_file_version);
        header_page.write_uint64_le(page_size, page_offset_page_size);
        header_page.write_uint64_le(1, page_offset_page_count);
        header_page.write_uint64_le(1, page_offset_page_id_last_empty);

        // Create an empty page as the second page in the file
        data_page.clear();

        // Create a circular reference to itself
        data_page.write_uint64_le(1);
    }

    page_view page_controller::internal_allocate_page(const std::size_t offset, const std::size_t size)
    {
        if ((offset % page_size) != 0)
        {
            throw std::runtime_error{ "Offset must be a multiple of page size" };
        }

        std::span<unsigned char> view = descriptor.acquire_view(offset, size);
        return page_view{ (offset / page_size) - 1, view };
    }
} // namespace data
