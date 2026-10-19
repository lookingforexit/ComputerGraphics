#include "camera.hpp"

#include <algorithm>
#include <cmath>
#include <imgui.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

namespace application {
glm::mat4 Camera::view_matrix() const {
    const float pitch = glm::radians(std::clamp(pitch_degrees, -89.0f, 89.0f));
    const float yaw = glm::radians(yaw_degrees);
    const glm::vec3 forward(std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                            std::cos(pitch) * std::sin(yaw));
    glm::vec3 up(0, 1, 0);
    const float roll = glm::radians(roll_degrees);
    up = glm::vec3(glm::rotate(glm::mat4(1.0f), roll, forward) * glm::vec4(up, 0.0f));
    return glm::lookAt(position, position + forward, up);
}

glm::mat4 Camera::projection_matrix(float aspect) const {
    glm::mat4 projection(1.0f);
    if (use_perspective) {
        projection = glm::perspective(glm::radians(field_of_view_degrees), aspect, near_clip, far_clip);
    } else {
        const float half_height = orthographic_half_height;
        const float half_width = half_height * aspect;
        projection = glm::ortho(-half_width, half_width, -half_height, half_height, near_clip, far_clip);
    }
    projection[1][1] *= -1.0f;
    return projection;
}

void Camera::update_from_keyboard(GLFWwindow *window, float delta_time) {
    if (window == nullptr || glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_FALSE ||
        ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }

    const float yaw = glm::radians(yaw_degrees);
    const glm::vec3 forward(std::cos(yaw), 0.0f, std::sin(yaw));
    const glm::vec3 right(-std::sin(yaw), 0.0f, std::cos(yaw));
    glm::vec3 movement(0.0f);
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        movement += forward;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        movement -= forward;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        movement += right;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        movement -= right;
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        movement.y += 1.0f;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) {
        movement.y -= 1.0f;
    }

    const float movement_length = glm::length(movement);
    if (movement_length > 1.0f) {
        movement /= movement_length;
    }
    position += movement * movement_speed * delta_time;

    const float turn = turn_speed_degrees * delta_time;
    if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) {
        yaw_degrees -= turn;
    }
    if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) {
        yaw_degrees += turn;
    }
    if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) {
        pitch_degrees += turn;
    }
    if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) {
        pitch_degrees -= turn;
    }
    pitch_degrees = std::clamp(pitch_degrees, -89.0f, 89.0f);
    yaw_degrees = std::remainder(yaw_degrees, 360.0f);
}

}
