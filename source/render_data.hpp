#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace graphics::core {

enum class MeshKind { Sphere, Plane };
enum class Shading { Lit, Unlit };

struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec3 color{1.0f};
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct Material {
    glm::vec4 albedo{1.0f};
    glm::vec3 specular_color{1.0f};
    float shininess = 32.0f;
    bool use_vertex_color = false;
};

struct AmbientLight {
    glm::vec3 color{0.12f};
    float intensity = 0.25f;
};

struct DirectionalLight {
    glm::vec3 direction{-0.5f, -1.0f, -0.3f};
    glm::vec3 color{1.0f, 0.95f, 0.85f};
    float intensity = 0.8f;
};

struct PointLight {
    glm::vec3 position{0.0f, 2.0f, 2.0f};
    glm::vec3 color{0.4f, 0.7f, 1.0f};
    float intensity = 18.0f;
};

struct SpotLight {
    glm::vec3 position{0.0f, 3.0f, 2.0f};
    glm::vec3 direction{0.0f, -1.0f, -0.5f};
    glm::vec3 color{1.0f, 0.65f, 0.3f};
    float intensity = 30.0f;
    float inner_angle_degrees = 18.0f;
    float outer_angle_degrees = 28.0f;
};

struct RenderObject {
    MeshKind mesh = MeshKind::Sphere;
    Shading shading = Shading::Lit;
    glm::mat4 model{1.0f};
    Material material;
};

struct RenderFrame {
    glm::mat4 view_projection{1.0f};
    glm::vec3 camera_position{};
    AmbientLight ambient;
    DirectionalLight directional;
    std::vector<RenderObject> objects;
    std::vector<PointLight> point_lights;
    std::vector<SpotLight> spot_lights;
    std::vector<Vertex> light_lines;
};

}
