#include "avis/runtime/io/file_descriptor.h"

namespace io
{
    std::size_t basic_file_descriptor::allocation_granularity_ = 0;
    std::once_flag basic_file_descriptor::static_initialization_flag_;

    basic_file_descriptor::basic_file_descriptor(const std::filesystem::path& file_path) :
        file_path_{file_path},
        file_handle_{},
        file_mapping_{},
        initialization_flag_{},
        active_views_{},
        active_views_mutex_{}
    {}

    basic_file_descriptor::basic_file_descriptor(const basic_file_descriptor& other) noexcept :
        file_path_{other.file_path_},
        file_handle_{},
        file_mapping_{},
        initialization_flag_{},
        active_views_{},
        active_views_mutex_{}
    {}

    basic_file_descriptor::basic_file_descriptor(basic_file_descriptor&& other) noexcept :
        file_path_{std::move(other.file_path_)},

        file_handle_{std::move(other.file_handle_)},
        file_mapping_{std::move(other.file_mapping_)},

        active_views_{std::move(other.active_views_)}
    {
        std::call_once(
            static_initialization_flag_,
            []()
            {
                SYSTEM_INFO system_info = {};
                ::GetSystemInfo(&system_info);

                allocation_granularity_ = system_info.dwAllocationGranularity;
            });

        other.file_path_.clear();

        other.active_views_.clear();
    }

    basic_file_descriptor::~basic_file_descriptor()
    {
        release_all_views();
    }

    std::span<unsigned char> basic_file_descriptor::acquire_view(std::size_t offset, std::size_t size)
    {
        // Lazily initialize the file handles
        if (!file_mapping_)
        {
            initialize_handles();
        }

        std::optional<std::span<unsigned char>> view = try_find_view(offset, size);
        if (view.has_value())
        {
            return view.value();
        }

        return create_view(offset, size);
    }

    void basic_file_descriptor::release_view(const std::span<unsigned char>& view)
    {
        std::scoped_lock<std::mutex> lock{active_views_mutex_};

        auto element_it = std::find_if(
            std::cbegin(active_views_),
            std::cend(active_views_),
            [view](const file_view& internal_view)
            {
                if (view.data() == internal_view.data.data() && view.size() == internal_view.data.size())
                {
                    return false;
                }

                return true;
            });
        if (element_it == std::end(active_views_))
        {
            return;
        }

        ::UnmapViewOfFile(element_it->data.data());
        active_views_.erase(element_it);
    }

    void basic_file_descriptor::release_all_views()
    {
        std::scoped_lock<std::mutex> lock{active_views_mutex_};

        for (auto& view : active_views_)
        {
            ::UnmapViewOfFile(view.data.data());
        }
        active_views_.clear();
    }

    void basic_file_descriptor::release()
    {
        release_all_views();

        file_path_ = std::filesystem::path{};
        file_mapping_.reset();
        file_handle_.reset();
    }

    void basic_file_descriptor::swap(basic_file_descriptor& other)
    {
        file_path_.swap(other.file_path_);

        file_handle_.swap(other.file_handle_);
        file_mapping_.swap(other.file_mapping_);

        active_views_.swap(other.active_views_);
    }

    basic_file_descriptor& basic_file_descriptor::operator=(const basic_file_descriptor& other) noexcept
    {
        basic_file_descriptor{other}.swap(*this);
        return *this;
    }

    basic_file_descriptor& basic_file_descriptor::operator=(basic_file_descriptor&& other) noexcept
    {
        basic_file_descriptor{std::move(other)}.swap(*this);
        return *this;
    }

    void basic_file_descriptor::initialize_handles()
    {
        std::call_once(
            initialization_flag_,
            [this]()
            {
                if (!file_mapping_)
                {
                    file_handle_.reset(::CreateFileW(
                        file_path_.c_str(),
                        GENERIC_READ,
                        FILE_SHARE_READ,
                        NULL,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL));
                    if (!file_handle_)
                    {
                        throw make_win32_error_code();
                    }

                    file_mapping_.reset(::CreateFileMapping(file_handle_.get(), NULL, PAGE_READONLY, 0, 0, NULL));
                    if (!file_mapping_)
                    {
                        throw make_win32_error_code();
                    }
                }
            });
    }

    std::optional<std::span<unsigned char>> basic_file_descriptor::try_find_view(std::size_t offset, std::size_t size)
    {
        std::scoped_lock<std::mutex> lock{active_views_mutex_};

        // Check if (enclosing) mapping already exists, and return existing or (sub)mapping if possible
        std::vector<file_view>::const_iterator iterator = std::find_if(
            std::cbegin(active_views_),
            std::cend(active_views_),
            [offset, size](const file_view& view)
            {
                // Check if the currently iterated view contains the requested view. I.e. [offset, offset + size) falls
                // within [view.offset, view.offset + view.size).
                if (view.offset > offset)
                {
                    return false;
                }
                if (view.size - (offset - view.offset) < size)
                {
                    return false;
                }

                return true;
            });
        if (iterator == std::cend(active_views_))
        {
            return std::nullopt;
        }

        return iterator->data.subspan(offset - iterator->offset, size);
    }

    std::span<unsigned char> basic_file_descriptor::create_view(std::size_t offset, std::size_t size)
    {
        std::size_t file_byte_size = file_size();

        // If no size was given (i.e. size of 0), adjust it to the end of the file starting from the given offset
        if (size == 0)
        {
            size = file_byte_size - offset;
        }

        // Make sure that offset and size are within the size of a given file. Throw error if view isn't valid with
        // regards to file size.
        check_range_against_file_size(file_byte_size, offset, size);

        // Offset must be a multiple of the system allocation granularity, so adjust it to be the largest multiple
        // that is less than or equal to the original offset. Also adjust the size of the range to include the offset
        // adjustment.
        std::size_t adjusted_offset = (offset / allocation_granularity_) * allocation_granularity_;
        std::size_t adjusted_size = size + offset - adjusted_offset;

        uint32_t offset_high = static_cast<uint32_t>((adjusted_offset >> 32) & 0xFFFFFFFF);
        uint32_t offset_low = static_cast<uint32_t>(adjusted_offset & 0xFFFFFFFF);
        void* contents = ::MapViewOfFile(file_mapping_.get(), FILE_MAP_READ, offset_high, offset_low, adjusted_size);
        if (contents == nullptr)
        {
            throw make_win32_error_code();
        }

        file_view view{adjusted_offset, adjusted_size, static_cast<unsigned char*>(contents)};
        std::span<unsigned char> data_view = view.data.subspan(offset - adjusted_offset, size);

        store_view(std::move(view));

        return data_view;
    }

    void basic_file_descriptor::store_view(file_view&& view)
    {
        std::scoped_lock<std::mutex> lock{active_views_mutex_};
        active_views_.push_back(std::forward<file_view>(view));
    }

    void basic_file_descriptor::check_range_against_file_size(
        std::size_t file_byte_size, std::size_t offset, std::size_t size)
    {
        if (offset >= file_byte_size)
        {
            throw std::invalid_argument("Offset cannot be larger than the file size.");
        }

        if (offset + size > file_byte_size)
        {
            throw std::invalid_argument("The range starting from offset and given size is invalid.");
        }
    }

    std::size_t basic_file_descriptor::file_size() const
    {
        LARGE_INTEGER file_size = {};
        auto succeeded = ::GetFileSizeEx(file_handle_.get(), &file_size);
        if (succeeded == FALSE)
        {
            throw make_win32_error_code();
        }

        return static_cast<std::size_t>(file_size.QuadPart);
    }

    void file_descriptor_manager::release_descriptor(const file_descriptor descriptor)
    {
        basic_file_descriptor* internal_descriptor = basic_file_descriptor::from_file_descriptor(descriptor);

        if (!is_managed_descriptor(internal_descriptor))
        {
            // TODO: Decide to throw an exception or make this function a noop. Also decide to make this check a debug
            // only check.
            throw std::runtime_error("Given descriptor is managed by a different descriptor manager.");
        }

        // TODO: Decide whether to reuse released descriptors to reduce memory thrashing
        internal_descriptor->release();
    }

    file_descriptor_manager::file_descriptor_manager() :
        current_descriptor_list_{allocate_descriptor_list()},
        next_descriptor_index_{0}
    {}

    file_descriptor file_descriptor_manager::create_descriptor(const std::filesystem::path& file_path)
    {
        std::scoped_lock<std::mutex> lock{current_descriptor_list_mutex_};

        if (next_descriptor_index_ >= descriptor_list_size)
        {
            std::scoped_lock<std::mutex> lock{used_descriptor_lists_mutex_};
            used_descriptor_lists_.push_back(std::move(current_descriptor_list_));

            current_descriptor_list_ = allocate_descriptor_list();
            next_descriptor_index_ = 0;
        }

        basic_file_descriptor* new_descriptor =
            initialize_descriptor_in_list(file_path, current_descriptor_list_, next_descriptor_index_);
        next_descriptor_index_ += 1;

        return basic_file_descriptor::to_file_descriptor(new_descriptor);
    }

    void file_descriptor_manager::update_descriptor(const file_descriptor descriptor)
    {
        basic_file_descriptor* internal_descriptor = basic_file_descriptor::from_file_descriptor(descriptor);

        if (!is_managed_descriptor(internal_descriptor))
        {
            // TODO: Decide to throw an exception or make this function a noop. Also decide to make this check a debug
            // only check.
            throw std::runtime_error("Given descriptor is managed by a different descriptor manager.");
        }

        // TODO: Update descriptor
    }

    void file_descriptor_manager::release_all_descriptors()
    {
        {
            std::scoped_lock<std::mutex> lock{current_descriptor_list_mutex_};

            release_descriptors_in_list(current_descriptor_list_);
            next_descriptor_index_ = 0;
        }

        {
            std::scoped_lock<std::mutex> lock{used_descriptor_lists_mutex_};

            for (descriptor_list& list : used_descriptor_lists_)
            {
                release_descriptors_in_list(list);
            }
            used_descriptor_lists_.clear();
        }
    }

    file_descriptor file_descriptor_manager::find_descriptor(const std::filesystem::path& file_path)
    {
        // Check if the given descriptor is in the currently active list of descriptors
        basic_file_descriptor* descriptor = find_descriptor_in_list(file_path, current_descriptor_list_);
        if (descriptor != nullptr)
        {
            return basic_file_descriptor::to_file_descriptor(descriptor);
        }

        // Check if the given descriptor is in any of the old lists of descriptors
        for (auto& list : used_descriptor_lists_)
        {
            basic_file_descriptor* descriptor = find_descriptor_in_list(file_path, list);
            if (descriptor != nullptr)
            {
                return basic_file_descriptor::to_file_descriptor(descriptor);
            }
        }

        return false;
    }

    file_descriptor_manager::descriptor_list file_descriptor_manager::allocate_descriptor_list()
    {
        return std::make_unique<basic_file_descriptor[]>(descriptor_list_size);
    }

    void file_descriptor_manager::release_descriptors_in_list(const descriptor_list& list)
    {
        std::for_each(
            &list[0], &list[descriptor_list_size], [](basic_file_descriptor& descriptor) { descriptor.release(); });
    }

    basic_file_descriptor* file_descriptor_manager::initialize_descriptor_in_list(
        const std::filesystem::path& file_path, const descriptor_list& list, const std::size_t index) const
    {
        basic_file_descriptor* descriptor = &list[index];
        *descriptor = std::move(basic_file_descriptor{file_path});
        return descriptor;
    }

    basic_file_descriptor* file_descriptor_manager::find_descriptor_in_list(
        const std::filesystem::path& file_path, const descriptor_list& list) const
    {
        for (std::size_t desc_index = 0; desc_index < descriptor_list_size; ++desc_index)
        {
            basic_file_descriptor* descriptor = &list[desc_index];
            if (file_path == descriptor->file_path())
            {
                return descriptor;
            }
        }

        return nullptr;
    }

    bool file_descriptor_manager::is_managed_descriptor(basic_file_descriptor* descriptor) const
    {
        auto is_in_range = [](const basic_file_descriptor* descriptor, const descriptor_list& list)
        { return (descriptor >= list.get()) && (descriptor <= list.get() + descriptor_list_size); };

        // Check if the given descriptor is in the currently active list of descriptors
        if (is_in_range(descriptor, current_descriptor_list_))
        {
            return true;
        }

        // Check if the given descriptor is in any of the old lists of descriptors
        for (auto& list : used_descriptor_lists_)
        {
            if (is_in_range(descriptor, list))
            {
                return true;
            }
        }

        return false;
    }
} // namespace io
