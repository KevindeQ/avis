#ifndef MIDDLEWARE_SINGLETON_H
#define MIDDLEWARE_SINGLETON_H

template<typename derived_type>
class singleton
{
public:
    singleton()
    {
        instance = static_cast<derived_type*>(this);
    }

    singleton(const singleton<derived_type>&) = delete;

    ~singleton()
    {
        instance = nullptr;
    }

    singleton& operator=(const singleton<derived_type>&) = delete;

    static derived_type& get_instance()
    {
        return *instance;
    }
    static derived_type* get_instance_ptr()
    {
        return instance;
    }

private:
    static derived_type* instance;
};

#endif
