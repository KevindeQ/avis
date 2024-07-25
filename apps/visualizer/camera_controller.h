#ifndef CAMERA_CONTROLLER_H
#define CAMERA_CONTROLLER_H

#include "avis/core/common.h"
#include "camera.h"

class camera_controller
{
public:
    camera_controller() noexcept;
    camera_controller(camera* const target);
    camera_controller(const camera_controller& other) noexcept;
    camera_controller(camera_controller&& other) noexcept;
    ~camera_controller() noexcept = default;

    camera_controller& operator=(const camera_controller& rhs) noexcept;
    camera_controller& operator=(camera_controller&& rhs) noexcept;

    void move_forward(const float amount, const std::chrono::duration<double> delta_time_seconds);
    void move_left(const float amount, const std::chrono::duration<double> delta_time_seconds);
    void move_upward(const float amount, const std::chrono::duration<double> delta_time_seconds);

    void update();

    void reset();

private:
    void clear_movement();

    void update_heading();
    void update_pitch();

    inline Eigen::Vector3f vector_north() const;
    inline Eigen::Vector3f vector_east() const;
    inline Eigen::Vector3f vector_up() const;

    camera* camera_target;
    Eigen::Vector3f position_initial;
    Eigen::Quaternionf rotation_initial;

    Eigen::Matrix3f matrix_basis_world;

    float forward;
    float strafe;
    float ascent;

    float current_heading;
    float current_pitch;

    float speed_move;
    float speed_strafe;
};

inline Eigen::Vector3f camera_controller::vector_north() const
{
    return -matrix_basis_world.row(2);
}

inline Eigen::Vector3f camera_controller::vector_east() const
{
    return matrix_basis_world.row(0);
}

inline Eigen::Vector3f camera_controller::vector_up() const
{
    return matrix_basis_world.row(1);
}

#endif
