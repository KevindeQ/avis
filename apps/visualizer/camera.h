#ifndef CAMERA_H
#define CAMERA_H

#include <Eigen/Dense>

// Implementation of the camera classes (basic_camera, camera and camera_controller) are based of information
// on the following webpage: https://gpuopen.com/learn/matrix-compendium/matrix-compendium-postmult_rh_gl/#camera-transformations.
class basic_camera
{
public:
    void look_at(const Eigen::Vector3f position_world, const Eigen::Vector3f target, const Eigen::Vector3f up);

    void update();

    const Eigen::Vector3f& position() const;
    void set_position(const Eigen::Vector3f position_world);

    const Eigen::Quaternionf& rotation() const;
    void set_rotation(const Eigen::Quaternionf rotation);

    const Eigen::Matrix4f& view_matrix() const;
    const Eigen::Matrix4f& projection_matrix() const;
    const Eigen::Matrix4f& view_projection_matrix() const;
    const Eigen::Matrix4f& previous_view_projection_matrix() const;
    const Eigen::Matrix4f& reprojection_matrix() const;

    const Eigen::Vector3f forward_vector() const;
    const Eigen::Vector3f right_vector() const;
    const Eigen::Vector3f up_vector() const;

    const Eigen::Matrix3f& basis() const;

protected:
    basic_camera();

    void set_matrix_projection(const Eigen::Matrix4f& new_matrix);

private:
    void set_look_direction(Eigen::Vector3f forward, Eigen::Vector3f up);

    Eigen::Quaternionf current_orientation;
    Eigen::Vector3f current_position;

    // Redundant data cached for faster lookups
    Eigen::Matrix3f matrix_basis;

    // Transforms homogeneous coordinates from world space to view space. In this case, view space is defined as +X to
    // the right, +Y is up, and -Z is forward. This corresponds with a right-handed coordinate system.
    Eigen::Matrix4f matrix_view; // i.e. "World-to-View" matrix

    // The projection matrix transforms view space to clip space. Once division by W has occurred, the final
    // coordinates can be transformed by the viewport matrix to screen space. The projection matrix is determined by
    // the screen aspect and camera field of view. A projection matrix can also be orthographic. In that case, field
    // of view would be defined in linear units, not angles.
    Eigen::Matrix4f matrix_projection; // i.e. "View-to-Projection" matrix

    // A concatenation of the view and projection matrices.
    Eigen::Matrix4f matrix_view_projection; // i.e.  "World-To-Projection" matrix.

    // The view-projection matrix from the previous frame
    Eigen::Matrix4f matrix_view_projection_previous;

    // Projects a clip-space coordinate to the previous frame (useful for temporal effects).
    Eigen::Matrix4f matrix_reproject;
};

class camera : public basic_camera
{
public:
    camera();
    camera(const float fov_vertical, const float width_over_height, const float clip_near, const float clip_far);

    void set_field_of_view(const float fov_vertical);
    void set_aspect_ratio(const float width_over_height);
    void set_z_range(const float clip_near, const float clip_far);

    float field_of_view() const;
    float clip_near() const;
    float clip_far() const;

private:
    void update_matrix_projection();

    float field_of_view_vertical;
    float aspect_ratio; // aspect ratio = height / width
    float clip_z_near;
    float clip_z_far;
};

#endif
