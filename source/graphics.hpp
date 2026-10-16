#pragma once

#include <cstdint>
#include <vector>

#include "graphics_internal.hpp"

namespace graphics::core {

// The application keeps the original three-object limit from Lab 1.
inline constexpr uint32_t kMaxObjectCount = 3;

// These structures are mirrored in GLSL. vec4-shaped arrays keep std140/std430
// alignment explicit and predictable across C++ compilers and Vulkan drivers.
struct SceneUniforms {
	float view_projection[4][4];
	float camera_position[4];
	float ambient_color_intensity[4];
	float directional_direction[4];
	float directional_color_intensity[4];
	uint32_t light_counts[4];
};

struct ModelUniforms {
	float model[4][4];
	float albedo[4];
	float specular[4];
	float material_parameters[4]; // x = shininess, y = unlit marker
};

struct Vertex {
	float position[3];
	float normal[3];
	float color[3];
};

struct PointLight {
	float position[4];
	float color_intensity[4];
};

struct SpotLight {
	float position[4];
	float direction[4];
	float color_intensity[4];
	float cone_angles[4]; // x = inner cosine, y = outer cosine
};

bool initialize();
void shutdown();

bool getGeometry(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
bool getEnvironmentGeometry(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

void render(const internal::FrameData& fd,
			const SceneUniforms& scene,
			const std::vector<ModelUniforms>& objects,
			const std::vector<PointLight>& point_lights,
			const std::vector<SpotLight>& spot_lights,
			const std::vector<Vertex>& visualization_lines);

} // namespace graphics::core
