#include "avis/runtime/data/point_cloud.h"

namespace data
{
    point_cloud::point_cloud(io::basic_file_descriptor& database_file, io::basic_file_descriptor& index_file) :
        point_data_provider{ database_file, index_file }
    {}

    std::vector<point_record> point_cloud::fetch_data(const Eigen::Vector3f& position)
    {
        /*std::vector<point_record> data = point_data_provider.query(query::contains(position));
        return data;*/

        std::vector<point_record> data = point_data_provider.query();
        return data;
    }
} // namespace data
