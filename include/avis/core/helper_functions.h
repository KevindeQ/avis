#ifndef CORE_HELPER_FUNCTIONS_H
#define CORE_HELPER_FUNCTIONS_H

template<typename T>
void safe_delete(T*& pointer)
{
    if (pointer != nullptr)
    {
        delete pointer;
        pointer = nullptr;
    }
}

template<typename T>
void safe_release(T*& pointer)
{
    if (pointer != nullptr)
    {
        pointer->Release();
        pointer = nullptr;
    }
}

inline void throw_if_failed(HRESULT result)
{
    if (FAILED(result))
    {
        throw;
    }
}

template<typename integer_t, std::enable_if_t<std::is_integral_v<integer_t>, bool> = true>
integer_t round_up(const integer_t value, const integer_t multiple)
{
    if (multiple == 0)
    {
        return value;
    }

    integer_t remainder = value % multiple;
    if (remainder == 0)
    {
        return value;
    }

    return value + multiple - remainder;
}

#endif
