#ifndef AVIS_TESTS_HELPERS_LOG_CAPTURE_H
#define AVIS_TESTS_HELPERS_LOG_CAPTURE_H

#include "avis/log.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <string>
#include <string_view>
#include <vector>

namespace avis::tests
{
    // Captures the messages of one named logger while it is alive, as "[<level>] <message>" lines.
    // The logger logs every level while a capture is attached, whether or not logging is initialised.
    class log_capture
    {
    public:
        explicit log_capture(std::string logger_name, std::size_t capacity = 1024) :
            logger_name_{ std::move(logger_name) },
            sink_{ std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(capacity) }
        {
            sink_->set_pattern("[%l] %v");
            avis::log::detail::attach_sink(logger_name_, sink_);
        }

        ~log_capture()
        {
            avis::log::detail::detach_sink(logger_name_, sink_);
        }

        log_capture(const log_capture&) = delete;
        log_capture& operator=(const log_capture&) = delete;

        std::vector<std::string> lines() const
        {
            auto lines = sink_->last_formatted();
            for (auto& line : lines)
            {
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                {
                    line.pop_back();
                }
            }
            return lines;
        }

        bool contains(const std::string_view text) const
        {
            const auto captured = lines();
            return std::ranges::any_of(captured, [text](const std::string& line) { return line.contains(text); });
        }

    private:
        std::string logger_name_;
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    };
} // namespace avis::tests

#endif
