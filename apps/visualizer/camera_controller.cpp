#include "camera_controller.h"

camera_controller::camera_controller() noexcept :
    camera_target{ nullptr },

    matrix_basis_world{ Eigen::Matrix3f::Identity() },

    forward{ 0.0f },
    strafe{ 0.0f },
    ascent{ 0.0f },

    current_heading{ 0.0f },
    current_pitch{ 0.0f },

    speed_move{ 0.0f },
    speed_strafe{ 0.0f }
{}

camera_controller::camera_controller(camera* const target) :
    camera_target{ target },
    position_initial{ Eigen::Vector3f::Zero() },
    rotation_initial{ Eigen::Quaternionf::Identity() },

    matrix_basis_world{ Eigen::Matrix3f::Zero() },

    forward{ 0.0f },
    strafe{ 0.0f },
    ascent{ 0.0f },

    current_heading{ 0.0f },
    current_pitch{ 0.0f },

    speed_move{ 1.4f },
    speed_strafe{ 1.4f }
{
    if (target == nullptr)
    {
        throw std::invalid_argument{ "target must not be a nullptr" };
    }

    // Store initial position and rotation so it can be reset to this
    position_initial = camera_target->position();
    rotation_initial = camera_target->rotation();

    Eigen::Vector3f world_up = Eigen::Vector3f::UnitY().normalized();
    Eigen::Vector3f world_north = world_up.cross(Eigen::Vector3f::UnitX()).normalized();
    Eigen::Vector3f world_east = world_north.cross(world_up).normalized();

    matrix_basis_world.row(0) = world_east;
    matrix_basis_world.row(1) = world_up;
    matrix_basis_world.row(2) = -world_north;

    update_heading();
    update_pitch();
}

camera_controller::camera_controller(const camera_controller& other) noexcept :
    camera_target{ other.camera_target },
    position_initial{ other.position_initial },
    rotation_initial{ other.rotation_initial },

    forward{ other.forward },
    strafe{ other.strafe },
    ascent{ other.ascent },

    current_heading{ other.current_heading },
    current_pitch{ other.current_pitch },

    speed_move{ other.speed_move },
    speed_strafe{ other.speed_strafe }
{}

camera_controller::camera_controller(camera_controller&& other) noexcept :
    camera_target{ std::exchange(other.camera_target, nullptr) },
    position_initial{ std::move(other.position_initial) },
    rotation_initial{ std::move(other.rotation_initial) },

    forward{ other.forward },
    strafe{ other.strafe },
    ascent{ other.ascent },

    current_heading{ other.current_heading },
    current_pitch{ other.current_pitch },

    speed_move{ other.speed_move },
    speed_strafe{ other.speed_strafe }
{}

camera_controller& camera_controller::operator=(const camera_controller& rhs) noexcept
{
    if (this != &rhs)
    {
        camera_target = rhs.camera_target;
        position_initial = rhs.position_initial;
        rotation_initial = rhs.rotation_initial;

        forward = rhs.forward;
        strafe = rhs.strafe;
        ascent = rhs.ascent;

        current_heading = rhs.current_heading;
        current_pitch = rhs.current_pitch;

        speed_move = rhs.speed_move;
        speed_strafe = rhs.speed_strafe;
    }

    return *this;
}

camera_controller& camera_controller::operator=(camera_controller&& rhs) noexcept
{
    if (this != &rhs)
    {
        camera_target = std::exchange(rhs.camera_target, nullptr);
        position_initial = std::move(rhs.position_initial);
        rotation_initial = std::move(rhs.rotation_initial);

        forward = rhs.forward;
        strafe = rhs.strafe;
        ascent = rhs.ascent;

        current_heading = rhs.current_heading;
        current_pitch = rhs.current_pitch;

        speed_move = rhs.speed_move;
        speed_strafe = rhs.speed_strafe;
    }

    return *this;
}

void camera_controller::move_forward(const float amount, const std::chrono::duration<double> delta_time_seconds)
{
    float ranged_amount = std::clamp(amount, -1.0f, 1.0f);
    forward = speed_move * ranged_amount * delta_time_seconds.count();
}

void camera_controller::move_left(const float amount, const std::chrono::duration<double> delta_time_seconds)
{
    float ranged_amount = std::clamp(amount, -1.0f, 1.0f);
    strafe = speed_strafe * -ranged_amount * delta_time_seconds.count();
}

void camera_controller::move_upward(const float amount, const std::chrono::duration<double> delta_time_seconds)
{
    float ranged_amount = std::clamp(amount, -1.0f, 1.0f);
    ascent = speed_strafe * ranged_amount * delta_time_seconds.count();
}

void camera_controller::update()
{
    if (camera_target == nullptr)
    {
        return;
    }

    Eigen::Matrix3f orientation = matrix_basis_world * Eigen::AngleAxis{ current_heading, Eigen::Vector3f::UnitY() } *
                                  Eigen::AngleAxis{ current_pitch, Eigen::Vector3f::UnitX() };
    Eigen::Vector3f position = orientation * Eigen::Vector3f(strafe, ascent, -forward) + camera_target->position();

    camera_target->set_position(position);
    camera_target->set_rotation(Eigen::Quaternionf{ orientation });
    camera_target->update();

    clear_movement();
}

void camera_controller::reset()
{
    camera_target->set_position(position_initial);
    camera_target->set_rotation(rotation_initial);

    update_heading();
    update_pitch();
}

void camera_controller::clear_movement()
{
    forward = 0.0f;
    strafe = 0.0f;
    ascent = 0.0f;
}

void camera_controller::update_heading()
{
    Eigen::Vector3f camera_right = camera_target->right_vector();
    Eigen::Vector3f forward = vector_up().cross(camera_right).normalized();
    current_heading = std::atan2(-forward.dot(vector_east()), forward.dot(vector_north()));
}

void camera_controller::update_pitch()
{
    current_pitch = std::sin(camera_target->forward_vector().dot(matrix_basis_world.row(1)));
}