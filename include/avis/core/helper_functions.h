#ifndef CORE_HELPER_FUNCTIONS_H
#define CORE_HELPER_FUNCTIONS_H

template<typename T>
void safe_delete(T*& pointer)
{
    if (pointer)
    {
        delete pointer;
        pointer = nullptr;
    }
}

template<typename T>
void safe_release(T*& pointer)
{
    if (pointer)
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

#endif
