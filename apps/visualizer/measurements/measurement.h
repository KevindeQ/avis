#ifndef MEASUREMENTS_MEASUREMENT_H
#define MEASUREMENTS_MEASUREMENT_H

#include "avis/common.h"

namespace measurements
{
    class measurement
    {
    public:

    private:
        std::chrono::steady_clock::time_point timestamp;
    };

    class measurement_imu : public measurement
    {

    };

    class measurement_gps : public measurement
    {

    };
} // namespace measurements

#endif
