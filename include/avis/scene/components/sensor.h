#ifndef AVIS_SCENE_COMPONENTS_SENSOR_H
#define AVIS_SCENE_COMPONENTS_SENSOR_H

// A sensor entity from the Driver (docs/design/08-scene-management.md §3): a child node of its vehicle, with the
// calibration extrinsic as the local pose. Intrinsics arrive as properties.

#include <cstdint>

namespace avis::scene::components
{
    enum class sensor_kind : std::uint8_t
    {
        camera,
        lidar,
        gnss_imu
    };

    struct sensor
    {
        sensor_kind kind = sensor_kind::camera;
    };
} // namespace avis::scene::components

#endif
