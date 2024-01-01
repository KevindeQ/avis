#ifndef RUNTIME_DATA_POINT_CLOUD_H
#define RUNTIME_DATA_POINT_CLOUD_H

#include "Eigen/Geometry"
#include "avis/core/common.h"
#include "avis/runtime/data/data_provider.h"
#include "avis/runtime/data/spatial_index.h"
#include "avis/runtime/io/file_descriptor.h"

namespace data
{
    struct point_record
    {
    public:
        friend class serializer<point_record>;

    private:
        Eigen::Vector3f position;
        float attenuation;
    };

    template<>
    class serializer<point_record>
    {
    public:
        template<typename storage_context>
        void serialize(storage_context& context)
        {
            context.serialize<&point_record::position>();
            context.serialize<&point_record::attenuation>();
        }
    };

    class point_cloud
    {
    public:
        point_cloud(io::basic_file_descriptor& database_file, io::basic_file_descriptor& index_file);

        std::vector<point_record> fetch_data(const Eigen::Vector3f& position);

    private:
        data_provider<point_record, spatial_index> point_data_provider;
    };
} // namespace data

#endif
