#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

namespace graphics::core::gpu {

inline constexpr uint32_t scene_binding = 0;
inline constexpr uint32_t point_lights_binding = 1;
inline constexpr uint32_t spot_lights_binding = 2;

struct alignas(16) SceneUniforms {
    glm::mat4 view_projection{1.0f};
    alignas(16) glm::vec3 camera_position{};
    float camera_padding = 0.0f;
    alignas(16) glm::vec3 ambient_color{};
    float ambient_intensity = 0.0f;
    alignas(16) glm::vec3 directional_direction{};
    float direction_padding = 0.0f;
    alignas(16) glm::vec3 directional_color{};
    float directional_intensity = 0.0f;
    uint32_t point_light_count = 0;
    uint32_t spot_light_count = 0;
    uint32_t count_padding[2]{};
};

struct alignas(16) ObjectUniforms {
    glm::mat4 model{1.0f};
    alignas(16) glm::vec4 albedo{1.0f};
    alignas(16) glm::vec3 specular_color{};
    float shininess = 32.0f;
    uint32_t use_vertex_color = 0;
    uint32_t material_padding[3]{};
};

struct alignas(16) PointLight {
    alignas(16) glm::vec3 position{};
    float position_padding = 0.0f;
    alignas(16) glm::vec3 color{};
    float intensity = 0.0f;
};

struct alignas(16) SpotLight {
    alignas(16) glm::vec3 position{};
    float position_padding = 0.0f;
    alignas(16) glm::vec3 direction{};
    float direction_padding = 0.0f;
    alignas(16) glm::vec3 color{};
    float intensity = 0.0f;
    float inner_cone_cosine = 0.0f;
    float outer_cone_cosine = 0.0f;
    float cone_padding[2]{};
};
}
