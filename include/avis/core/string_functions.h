#ifndef CORE_STRING_FUNCTIONS_H
#define CORE_STRING_FUNCTIONS_H

// Trim from start (in place)
inline void trim_left(std::string& s)
{
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
}

// Trim from end (in place)
inline void trim_right(std::string& s)
{
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
}

// Trim from both ends (in place)
inline void trim(std::string& s)
{
    trim_left(s);
    trim_right(s);
}

inline std::vector<std::string_view> split(const std::string_view s, const std::string_view delimiter)
{
    std::vector<std::string_view> parts{};

    std::size_t pos = 0;
    std::size_t previous_pos = pos;
    while ((pos = s.find(delimiter, pos)) != std::string::npos)
    {
        std::string_view part = s.substr(previous_pos, pos - previous_pos);
        parts.emplace_back(part);

        // Also skip over the delimiter
        pos += delimiter.length();
        previous_pos = pos;
    }

    // Add the last part to the results
    parts.emplace_back(s.substr(previous_pos));

    return parts;
}

#endif
