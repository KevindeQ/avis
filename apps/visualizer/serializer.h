#ifndef VISUALIZER_SERIALIZER_H
#define VISUALIZER_SERIALIZER_H

namespace detail
{
    template<typename T, typename serializable_data_view_t>
    concept serializable_internal = requires(T t, serializable_data_view_t& v) {
        { t.template serialize<serializable_data_view_t>(v) };
    };

    template<typename T, typename serializable_data_view_t>
    concept serializable_external = requires(T t, serializable_data_view_t& v) {
        { serialize<serializable_data_view_t>(t, v) };
    };

    template<typename serializable_data_view_t>
    struct serialize_implementation
    {
    public:
        template<serializable_internal<serializable_data_view_t> T>
        void operator()(T& obj, serializable_data_view_t& view) const
        {
            obj.serialize(view);
        }

        template<serializable_external<serializable_data_view_t> T>
        void operator()(T& obj, serializable_data_view_t& view) const
        {
            serialize(obj, view);
        }
    };

    template<typename serializable_data_view_t>
    inline constexpr serialize_implementation<serializable_data_view_t> serialize{};
} // namespace detail

template<typename serializable_data_view_t>
class serializer
{
public:
    serializer(serializable_data_view_t& view) : view_{ view } {}

    template<typename T>
    void serialize(T& obj) const
    {
        detail::serialize<serializable_data_view_t>(obj, view_);
    }

private:
    serializable_data_view_t& view_;
};

#endif
