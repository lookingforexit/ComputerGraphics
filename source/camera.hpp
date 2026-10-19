#pragma once

#include <glm/glm.hpp>

struct GLFWwindow;

namespace application {

struct Camera {
    glm::vec3 position{0.0f, 0.0f, 7.0f};
    float pitch_degrees = 0.0f;
    float yaw_degrees = -90.0f;
    float roll_degrees = 0.0f;
    float field_of_view_degrees = 60.0f;
    float movement_speed = 4.0f;
    float turn_speed_degrees = 75.0f;
    float near_clip = 0.1f;
    float far_clip = 100.0f;
    float orthographic_half_height = 2.5f;
    bool use_perspective = true;

    glm::mat4 view_matrix() const;
    glm::mat4 projection_matrix(float aspect) const;
    void update_from_keyboard(GLFWwindow *window, float delta_time);
};

}
