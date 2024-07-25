#include "camera.h"

#include "avis/core/common.h"

void basic_camera::look_at(const Eigen::Vector3f position_world, const Eigen::Vector3f target, const Eigen::Vector3f up)
{
    set_look_direction(target - position_world, up);
    set_position(position_world);
}

const Eigen::Vector3f& basic_camera::position() const
{
    return current_position;
}

void basic_camera::set_position(const Eigen::Vector3f position_world)
{
    current_position = position_world;
}

const Eigen::Quaternionf& basic_camera::rotation() const
{
    return current_orientation;
}

void basic_camera::set_rotation(const Eigen::Quaternionf rotation)
{
    current_orientation = rotation.normalized();
    matrix_basis = current_orientation.toRotationMatrix();
}

void basic_camera::update()
{
    matrix_view_projection_previous = matrix_view_projection;

    Eigen::Quaternionf rotation_inverse = current_orientation.conjugate();
    Eigen::Vector3f translation_inverse = rotation_inverse * -current_position;
    Eigen::Matrix3f matrix_rotation = rotation_inverse.toRotationMatrix();

    matrix_view = Eigen::Matrix4f::Identity();
    matrix_view.block<3, 3>(0, 0) = matrix_rotation;
    matrix_view.block<3, 1>(0, 3) = translation_inverse;
    matrix_view.transposeInPlace();

    matrix_view_projection = matrix_projection * matrix_view;
    matrix_reproject = matrix_view_projection_previous * matrix_view_projection.inverse();
}

const Eigen::Matrix4f& basic_camera::view_matrix() const
{
    return matrix_view;
}

const Eigen::Matrix4f& basic_camera::projection_matrix() const
{
    return matrix_projection;
}

const Eigen::Matrix4f& basic_camera::view_projection_matrix() const
{
    return matrix_view_projection;
}

const Eigen::Matrix4f& basic_camera::previous_view_projection_matrix() const
{
    return matrix_view_projection_previous;
}

const Eigen::Matrix4f& basic_camera::reprojection_matrix() const
{
    return matrix_reproject;
}

basic_camera::basic_camera() :
    current_orientation{ Eigen::Quaternionf::Identity() },
    current_position{ Eigen::Vector3f::Zero() },

    matrix_basis{ Eigen::Matrix3f::Identity() },

    matrix_view{ Eigen::Matrix4f::Identity() },
    matrix_projection{ Eigen::Matrix4f::Identity() },
    matrix_view_projection{ Eigen::Matrix4f::Identity() },
    matrix_view_projection_previous{ Eigen::Matrix4f::Identity() },
    matrix_reproject{ Eigen::Matrix4f::Identity() }
{}

void basic_camera::set_matrix_projection(const Eigen::Matrix4f& new_matrix)
{
    matrix_projection = new_matrix;
}

void basic_camera::set_look_direction(Eigen::Vector3f forward, Eigen::Vector3f up)
{
    float forward_norm_squared = forward.squaredNorm();
    if (forward_norm_squared >= 0.000001f)
    {
        forward = (1.0f / std::sqrt(forward_norm_squared)) * forward;
    }
    else
    {
        forward = -Eigen::Vector3f::UnitZ();
    }

    Eigen::Vector3f right = up.cross(forward);
    float left_norm_squared = right.squaredNorm();
    if (left_norm_squared >= 0.000001f)
    {
        right = (1.0f / left_norm_squared) * right;
    }
    else
    {
        Eigen::Vector3f unit_y = Eigen::Vector3f::UnitY();
        right = Eigen::Quaternionf{ -(std::numbers::pi / 2.0f), unit_y.x(), unit_y.y(), unit_y.z() } * forward;
    }

    up = forward.cross(right);

    // clang-format off
    matrix_basis = Eigen::Matrix3f{
        { right.x(),   right.y(),   right.z()   },
        { up.x(),      up.y(),      up.z()      },
        { forward.x(), forward.y(), forward.z() }
    };
    // clang-format on

    set_rotation(Eigen::Quaternionf{ matrix_basis });
}

const Eigen::Vector3f basic_camera::forward_vector() const
{
    return matrix_basis.row(2);
}

const Eigen::Vector3f basic_camera::right_vector() const
{
    return matrix_basis.row(0);
}

const Eigen::Vector3f basic_camera::up_vector() const
{
    return matrix_basis.row(1);
}

const Eigen::Matrix3f& basic_camera::basis() const
{
    return matrix_basis;
}

camera::camera() : camera(std::numbers::pi / 2.0f, 1.0f, 0.1f, 100.0f) {}

camera::camera(const float fov_vertical, const float width_over_height, const float clip_near, const float clip_far) :
    basic_camera(),

    field_of_view_vertical{ fov_vertical },
    aspect_ratio{ width_over_height },
    clip_z_near{ clip_near },
    clip_z_far{ clip_far }
{
    update_matrix_projection();
}

void camera::set_field_of_view(const float fov_vertical)
{
    field_of_view_vertical = fov_vertical;
    update_matrix_projection();
}

void camera::set_aspect_ratio(const float width_over_height)
{
    aspect_ratio = width_over_height;
    update_matrix_projection();
}

void camera::set_z_range(const float clip_near, const float clip_far)
{
    clip_z_near = clip_near;
    clip_z_far = clip_far;
    update_matrix_projection();
}

float camera::field_of_view() const
{
    return field_of_view_vertical;
}

float camera::clip_near() const
{
    return clip_z_near;
}

float camera::clip_far() const
{
    return clip_z_far;
}

void camera::update_matrix_projection()
{
    const float y = 1.0f * std::tan(0.5f * field_of_view_vertical);
    const float x = y * aspect_ratio;

    const float q1 = (-clip_z_far) / (clip_z_far - clip_z_near);
    const float q2 = (-1.0f * clip_z_far * clip_z_near) / (clip_z_far - clip_z_near);

    // clang-format off

    set_matrix_projection(Eigen::Matrix4f{
        { x,    0.0f,  0.0f, 0.0f },
        { 0.0f, y,     0.0f, 0.0f },
        { 0.0f, 0.0f,  q1,   q2   },
        { 0.0f, 0.0f, -1.0f, 0.0f }
    });

    // clang-format on
}
