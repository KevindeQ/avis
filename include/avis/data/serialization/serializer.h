#ifndef DATA_SERIALIZATION_SERIALIZER_H
#define DATA_SERIALIZATION_SERIALIZER_H

namespace data
{
    namespace details
    {
        template<typename serializable_t, typename serializable_memory_view>
        concept serializable_internal = requires(serializable_t t, serializable_memory_view& v) {
            { t.serialize<serializable_memory_view>(v) };
        };

        template<typename serializable_t, typename serializable_memory_view>
        concept serializable_external = requires(serializable_t t, serializable_memory_view& v) {
            { serialize<serializable_memory_view>(t, v) };
        };

        template<typename serializable_memory_view>
        struct serialize_impl
        {
        public:
            template<serializable_internal<serializable_memory_view> serializable_t>
            void operator()(const serializable_t& obj, serializable_memory_view& view) const;

            template<serializable_external<serializable_memory_view> serializable_t>
            void operator()(const serializable_t& obj, serializable_memory_view& view) const;
        };

        template<typename serializable_memory_view>
        template<serializable_internal<serializable_memory_view> serializable_t>
        void serialize_impl<serializable_memory_view>::operator()(
            const serializable_t& obj, serializable_memory_view& view) const
        {
            obj.serialize(view);
        }

        template<typename serializable_memory_view>
        template<serializable_external<serializable_memory_view> serializable_t>
        void serialize_impl<serializable_memory_view>::operator()(
            const serializable_t& obj, serializable_memory_view& view) const
        {
            serialize(obj, view);
        }

        template<typename serializable_memory_view>
        inline constexpr serialize_impl<serializable_memory_view> serialize{};
    } // namespace details

    template<typename serializable_memory_view>
    class serializer
    {
    public:
        serializer(serializable_memory_view& view);

        template<typename T>
        void serialize(const T& obj) const;

    private:
        serializable_memory_view& _view;
    };

    template<typename serializable_memory_view>
    serializer<serializable_memory_view>::serializer(serializable_memory_view& view) : _view{ view }
    {}

    template<typename serializable_memory_view>
    template<typename T>
    void serializer<serializable_memory_view>::serialize(const T& obj) const
    {
        details::serialize<serializable_memory_view>(obj, _view);
    }
} // namespace data

#endif
