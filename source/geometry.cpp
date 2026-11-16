#include "geometry.hpp"

#include <cmath>
#include <glm/gtc/constants.hpp>

namespace geometry {
namespace {
glm::vec3 safe_normalize(const glm::vec3 &v, const glm::vec3 &fallback) {
    constexpr float minimum_length_squared = 1e-8f;
    const float length_squared = glm::dot(v, v);
    return length_squared > minimum_length_squared ? v / std::sqrt(length_squared) : fallback;
}

}

graphics::core::MeshData make_sphere(uint32_t longitude_segments, uint32_t latitude_segments) {
    graphics::core::MeshData mesh;
    if (longitude_segments < 3 || latitude_segments < 2) {
        return mesh;
    }
    auto &vertices = mesh.vertices;
    auto &indices = mesh.indices;
    vertices.reserve((latitude_segments + 1) * (longitude_segments + 1));
    indices.reserve(6 * longitude_segments * (latitude_segments - 1));
    for (uint32_t lat = 0; lat <= latitude_segments; ++lat) {
        const float v = static_cast<float>(lat) / latitude_segments;
        const float theta = glm::pi<float>() * v;
        for (uint32_t lon = 0; lon <= longitude_segments; ++lon) {
            const float u = static_cast<float>(lon) / longitude_segments;
            const float phi = glm::two_pi<float>() * u;
            const glm::vec3 position{std::sin(theta) * std::cos(phi), std::cos(theta),
                                     std::sin(theta) * std::sin(phi)};
            vertices.push_back({.position = position, .normal = position,
                                .texcoord = {u, v}});
        }
    }
    const uint32_t stride = longitude_segments + 1;
    for (uint32_t lat = 0; lat < latitude_segments; ++lat) {
        for (uint32_t lon = 0; lon < longitude_segments; ++lon) {
            const uint32_t upper = lat * stride + lon;
            const uint32_t lower = upper + stride;
            if (lat != 0) {
                indices.insert(indices.end(), {upper, upper + 1, lower});
            }
            if (lat + 1 != latitude_segments) {
                indices.insert(indices.end(), {upper + 1, lower + 1, lower});
            }
        }
    }
    return mesh;
}

graphics::core::MeshData make_plane() {
    graphics::core::MeshData mesh;
    mesh.vertices = {
        {.position = {-1, 0, -1}, .normal = {0, 1, 0}, .texcoord = {0, 0}},
        {.position = {1, 0, -1}, .normal = {0, 1, 0}, .texcoord = {4, 0}},
        {.position = {1, 0, 1}, .normal = {0, 1, 0}, .texcoord = {4, 4}},
        {.position = {-1, 0, 1}, .normal = {0, 1, 0}, .texcoord = {0, 4}},
    };
    mesh.indices = {0, 2, 1, 0, 3, 2};
    return mesh;
}

void append_spotlight_cone(std::vector<graphics::core::Vertex> &lines,
                           const graphics::core::SpotLight &light) {
    const auto add_line = [&lines](const glm::vec3 &start, const glm::vec3 &end) {
        lines.push_back({.position = start});
        lines.push_back({.position = end});
    };

    constexpr int ray_count = 8;
    constexpr int ring_segments = 32;
    constexpr float cone_length = 3.0f;
    constexpr float parallel_axis_threshold = 0.95f;
    lines.reserve(lines.size() + 2 * (ray_count + ring_segments));
    const glm::vec3 direction = safe_normalize(light.direction, {0, -1, 0});
    const glm::vec3 reference =
        std::abs(direction.y) > parallel_axis_threshold ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    const glm::vec3 right = safe_normalize(glm::cross(direction, reference), {1, 0, 0});
    const glm::vec3 up = glm::cross(right, direction);
    const float cone_radius = cone_length * std::tan(glm::radians(light.outer_angle_degrees));

    for (int ray = 0; ray < ray_count; ++ray) {
        const float angle = 2.0f * glm::pi<float>() * ray / ray_count;
        const glm::vec3 radial = right * std::cos(angle) + up * std::sin(angle);
        const glm::vec3 end = light.position + direction * cone_length + radial * cone_radius;
        add_line(light.position, end);
    }

    const glm::vec3 ring_center = light.position + direction * cone_length;
    for (int segment = 0; segment < ring_segments; ++segment) {
        const float start_angle = 2.0f * glm::pi<float>() * segment / ring_segments;
        const float end_angle = 2.0f * glm::pi<float>() * (segment + 1) / ring_segments;
        const glm::vec3 start = ring_center + cone_radius * (right * std::cos(start_angle) +
                                                             up * std::sin(start_angle));
        const glm::vec3 end = ring_center + cone_radius * (right * std::cos(end_angle) +
                                                           up * std::sin(end_angle));
        add_line(start, end);
    }
}

}
