#ifndef AVIS_UTILS_OVERLOAD_H
#define AVIS_UTILS_OVERLOAD_H

namespace avis::utils
{
    template<class... Ts>
    struct overload : Ts...
    {
        using Ts::operator()...;

        // Prevent implicit type conversions
        template<typename T>
        consteval void operator()(T) const
        {
            static_assert(false, "Unsupported type");
        }
    };
} // namespace avis::utils

#endif
