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
    vertices.reserve(2 + (latitude_segments - 1) * longitude_segments);
    indices.reserve(6 * longitude_segments * (latitude_segments - 1));

    const auto add_vertex = [&vertices](const glm::vec3 &position) {
        vertices.push_back({
            .position = position,
            .normal = position,
            .color = (position + glm::vec3(1.0f)) * 0.5f,
        });
    };

    add_vertex({0.0f, 1.0f, 0.0f});
    for (uint32_t lat = 1; lat < latitude_segments; ++lat) {
        const float theta = glm::pi<float>() * static_cast<float>(lat) / latitude_segments;
        for (uint32_t lon = 0; lon < longitude_segments; ++lon) {
            const float phi = 2.0f * glm::pi<float>() * static_cast<float>(lon) / longitude_segments;
            add_vertex({std::sin(theta) * std::cos(phi), std::cos(theta),
                        std::sin(theta) * std::sin(phi)});
        }
    }

    const uint32_t bottom = static_cast<uint32_t>(vertices.size());
    add_vertex({0.0f, -1.0f, 0.0f});

    for (uint32_t lon = 0; lon < longitude_segments; ++lon) {
        const uint32_t next = (lon + 1) % longitude_segments;
        indices.insert(indices.end(), {0, lon + 1, next + 1});
    }
    for (uint32_t ring = 0; ring < latitude_segments - 2; ++ring) {
        for (uint32_t lon = 0; lon < longitude_segments; ++lon) {
            const uint32_t next = (lon + 1) % longitude_segments;
            const uint32_t upper_left = ring * longitude_segments + lon + 1;
            const uint32_t upper_right = ring * longitude_segments + next + 1;
            const uint32_t lower_left = upper_left + longitude_segments;
            const uint32_t lower_right = upper_right + longitude_segments;
            indices.insert(indices.end(), {upper_left, upper_right, lower_left,
                                            upper_right, lower_right, lower_left});
        }
    }
    const uint32_t last_ring = bottom - longitude_segments;
    for (uint32_t lon = 0; lon < longitude_segments; ++lon) {
        const uint32_t next = (lon + 1) % longitude_segments;
        indices.insert(indices.end(), {bottom, last_ring + lon, last_ring + next});
    }
    return mesh;
}

graphics::core::MeshData make_plane() {
    graphics::core::MeshData mesh;
    mesh.vertices = {
        {.position = {-1, 0, -1}, .normal = {0, 1, 0}, .color = {1, 1, 1}},
        {.position = {1, 0, -1}, .normal = {0, 1, 0}, .color = {1, 1, 1}},
        {.position = {1, 0, 1}, .normal = {0, 1, 0}, .color = {1, 1, 1}},
        {.position = {-1, 0, 1}, .normal = {0, 1, 0}, .color = {1, 1, 1}},
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
