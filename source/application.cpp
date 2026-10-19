#include "application.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "graphics.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"
#include "glm/ext/matrix_clip_space.hpp"
#include "glm/ext/matrix_transform.hpp"
#include "glm/ext/scalar_constants.hpp"
#include "glm/gtc/matrix_inverse.hpp"
#include "glm/gtc/type_ptr.hpp"

namespace application {
namespace {
	constexpr uint32_t longitude = 120;
	constexpr uint32_t latitude = 100;

	struct ObjectState {
		glm::vec3 position{};
		glm::vec3 rotation{};
		glm::vec3 scale{0.75f};
		glm::vec4 color{1.0f};
		glm::vec3 specular{1.0f};
		float shininess = 32.0f;
		float phase = 0.0f;
	};

	struct CameraState {
		glm::vec3 position{0.0f, 0.0f, 7.0f};
		glm::vec3 rotation_degrees{0.0f, -90.0f, 0.0f};
		float field_of_view = 60.0f;
		float movement_speed = 4.0f;
		float turn_speed = 75.0f;
	};

	struct PointLightState {
		glm::vec3 position{};
		glm::vec3 color{1.0f};
		float intensity = 18.0f;
	};

	struct SpotLightState {
		glm::vec3 position{};
		glm::vec3 direction{0.0f, -1.0f, 0.0f};
		glm::vec3 color{1.0f};
		float intensity = 30.0f;
		float inner_angle = 18.0f;
		float outer_angle = 28.0f;
	};

	std::array<ObjectState, graphics::core::kMaxObjectCount> objects{};
	CameraState camera;
	std::vector<PointLightState> point_lights;
	std::vector<SpotLightState> spot_lights;
	glm::vec3 ambient_color{0.12f};
	float ambient_intensity = 0.25f;
	glm::vec3 directional_direction{-0.5f, -1.0f, -0.3f};
	glm::vec3 directional_color{1.0f, 0.95f, 0.85f};
	float directional_intensity = 0.8f;

	bool use_perspective = true;
	bool animation_playing = true;
	bool show_light_markers = true;
	float animation_speed = 1.0f;
	float orbit_radius = 0.35f;
	float rotation_speed = 0.6f;
	int selected_object = 0;
	double previous_time = 0.0;
	double animation_time = 0.0;

	glm::vec3 safeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
		const float length_squared = glm::dot(v, v);
		return length_squared > 1e-8f ? v / std::sqrt(length_squared) : fallback;
	}

	void createSphere(std::vector<graphics::core::Vertex>& vertices,
				   std::vector<uint32_t>& indices) {
		vertices.clear();
		indices.clear();
		vertices.reserve(2 + (latitude - 1) * longitude);
		indices.reserve(6 * longitude * (latitude - 1));

		const auto add_vertex = [&vertices](const glm::vec3& p) {
			vertices.push_back({
				.position = {p.x, p.y, p.z},
				.normal = {p.x, p.y, p.z},
				.color = {0.5f + 0.5f * p.x, 0.5f + 0.5f * p.y, 0.5f + 0.5f * p.z}
			});
		};

		add_vertex({0.0f, 1.0f, 0.0f});
		for (uint32_t lat = 1; lat < latitude; ++lat) {
			const float theta = glm::pi<float>() * static_cast<float>(lat) / latitude;
			for (uint32_t lon = 0; lon < longitude; ++lon) {
				const float phi = 2.0f * glm::pi<float>() * static_cast<float>(lon) / longitude;
				add_vertex({std::sin(theta) * std::cos(phi), std::cos(theta),
							std::sin(theta) * std::sin(phi)});
			}
		}

		const uint32_t bottom = static_cast<uint32_t>(vertices.size());
		add_vertex({0.0f, -1.0f, 0.0f});

		for (uint32_t lon = 0; lon < longitude; ++lon) {
			const uint32_t next = (lon + 1) % longitude;
			indices.insert(indices.end(), {0, lon + 1, next + 1});
		}
		for (uint32_t ring = 0; ring < latitude - 2; ++ring) {
			for (uint32_t lon = 0; lon < longitude; ++lon) {
				const uint32_t next = (lon + 1) % longitude;
				const uint32_t a = ring * longitude + lon + 1;
				const uint32_t b = ring * longitude + next + 1;
				const uint32_t c = a + longitude;
				const uint32_t d = b + longitude;
				indices.insert(indices.end(), {a, b, c, b, d, c});
			}
		}
		const uint32_t last_ring = bottom - longitude;
		for (uint32_t lon = 0; lon < longitude; ++lon) {
			const uint32_t next = (lon + 1) % longitude;
			indices.insert(indices.end(), {bottom, last_ring + lon, last_ring + next});
		}
	}

	void createPlane(std::vector<graphics::core::Vertex>& vertices,
				  std::vector<uint32_t>& indices) {
		vertices = {
			{.position = {-1, 0, -1}, .normal = {0, 1, 0}, .color = {1, 1, 1}},
			{.position = { 1, 0, -1}, .normal = {0, 1, 0}, .color = {1, 1, 1}},
			{.position = { 1, 0,  1}, .normal = {0, 1, 0}, .color = {1, 1, 1}},
			{.position = {-1, 0,  1}, .normal = {0, 1, 0}, .color = {1, 1, 1}},
		};
		indices = {0, 2, 1, 0, 3, 2};
	}

	void copyMatrix(const glm::mat4& source, float destination[4][4]) {
		std::memcpy(destination, glm::value_ptr(source), sizeof(float) * 16);
	}

	glm::mat4 makeModelMatrix(const ObjectState& object, double time) {
		const float t = static_cast<float>(time);
		glm::vec3 position = object.position;
		position.x += orbit_radius * std::cos(t * animation_speed + object.phase);
		position.y += orbit_radius * std::sin(t * animation_speed + object.phase);

		glm::mat4 model(1.0f);
		model = glm::translate(model, position);
		model = glm::rotate(model, object.rotation.x + t * rotation_speed, {1, 0, 0});
		model = glm::rotate(model, object.rotation.y + t * rotation_speed, {0, 1, 0});
		model = glm::rotate(model, object.rotation.z, {0, 0, 1});
		return glm::scale(model, object.scale);
	}

	glm::mat4 makeViewMatrix() {
		const float pitch = glm::radians(std::clamp(camera.rotation_degrees.x, -89.0f, 89.0f));
		const float yaw = glm::radians(camera.rotation_degrees.y);
		const glm::vec3 forward(std::cos(pitch) * std::cos(yaw), std::sin(pitch),
								std::cos(pitch) * std::sin(yaw));
		glm::vec3 up(0, 1, 0);
		const float roll = glm::radians(camera.rotation_degrees.z);
		up = glm::vec3(glm::rotate(glm::mat4(1.0f), roll, forward) * glm::vec4(up, 0.0f));
		return glm::lookAt(camera.position, camera.position + forward, up);
	}

	glm::mat4 makeProjectionMatrix(float aspect) {
		glm::mat4 projection(1.0f);
		if (use_perspective) {
			projection = glm::perspective(glm::radians(camera.field_of_view), aspect, 0.1f, 100.0f);
		} else {
			const float half_height = 2.5f;
			const float half_width = half_height * aspect;
			projection = glm::ortho(-half_width, half_width, -half_height, half_height, 0.1f, 100.0f);
		}
		projection[1][1] *= -1.0f;
		return projection;
	}

	void drawLightControls() {
		if (ImGui::CollapsingHeader("Ambient light", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::ColorEdit3("Ambient color", glm::value_ptr(ambient_color));
			ImGui::SliderFloat("Ambient intensity", &ambient_intensity, 0.0f, 2.0f);
		}
		if (ImGui::CollapsingHeader("Directional light", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::DragFloat3("Direction", glm::value_ptr(directional_direction), 0.01f, -1.0f, 1.0f);
			ImGui::ColorEdit3("Directional color", glm::value_ptr(directional_color));
			ImGui::SliderFloat("Directional intensity", &directional_intensity, 0.0f, 5.0f);
		}
		if (ImGui::CollapsingHeader("Point lights", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::Button("Add point light")) {
				point_lights.push_back({{0, 2, 2}, {0.4f, 0.7f, 1.0f}, 18.0f});
			}
			for (size_t i = 0; i < point_lights.size(); ++i) {
				auto& light = point_lights[i];
				ImGui::PushID(static_cast<int>(i));
				if (ImGui::TreeNode("point", "Point light %zu", i + 1)) {
					ImGui::DragFloat3("Position", glm::value_ptr(light.position), 0.05f);
					ImGui::ColorEdit3("Color", glm::value_ptr(light.color));
					ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 100.0f);
					if (ImGui::Button("Remove point light")) {
						point_lights.erase(point_lights.begin() + static_cast<std::ptrdiff_t>(i));
						ImGui::TreePop(); ImGui::PopID(); break;
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
		}
		if (ImGui::CollapsingHeader("Spot lights", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::Button("Add spot light")) {
				spot_lights.push_back({{0, 3, 2}, {0, -1, -0.5f}, {1.0f, 0.65f, 0.3f}, 30, 18, 28});
			}
			for (size_t i = 0; i < spot_lights.size(); ++i) {
				auto& light = spot_lights[i];
				ImGui::PushID(static_cast<int>(i));
				if (ImGui::TreeNode("spot", "Spot light %zu", i + 1)) {
					ImGui::DragFloat3("Position", glm::value_ptr(light.position), 0.05f);
					ImGui::DragFloat3("Direction", glm::value_ptr(light.direction), 0.01f, -1, 1);
					ImGui::ColorEdit3("Color", glm::value_ptr(light.color));
					ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 100.0f);
					ImGui::SliderFloat("Inner angle", &light.inner_angle, 0.0f, light.outer_angle);
					ImGui::SliderFloat("Outer angle", &light.outer_angle, light.inner_angle, 89.0f);
					if (ImGui::Button("Remove spot light")) {
						spot_lights.erase(spot_lights.begin() + static_cast<std::ptrdiff_t>(i));
						ImGui::TreePop(); ImGui::PopID(); break;
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
		}
	}

	void updateCameraFromKeyboard(GLFWwindow* window, float delta_time) {
		if (window == nullptr || glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_FALSE ||
			ImGui::GetIO().WantCaptureKeyboard) {
			return;
		}

		const float yaw = glm::radians(camera.rotation_degrees.y);
		const glm::vec3 forward(std::cos(yaw), 0.0f, std::sin(yaw));
		const glm::vec3 right(-std::sin(yaw), 0.0f, std::cos(yaw));
		glm::vec3 movement(0.0f);
		if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) movement += forward;
		if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) movement -= forward;
		if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) movement += right;
		if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) movement -= right;
		if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) movement.y += 1.0f;
		if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
			glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) movement.y -= 1.0f;

		const float movement_length = glm::length(movement);
		if (movement_length > 1.0f) movement /= movement_length;
		camera.position += movement * camera.movement_speed * delta_time;

		const float turn = camera.turn_speed * delta_time;
		if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) camera.rotation_degrees.y -= turn;
		if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) camera.rotation_degrees.y += turn;
		if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) camera.rotation_degrees.x += turn;
		if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) camera.rotation_degrees.x -= turn;
		camera.rotation_degrees.x = std::clamp(camera.rotation_degrees.x, -89.0f, 89.0f);
		camera.rotation_degrees.y = std::remainder(camera.rotation_degrees.y, 360.0f);
	}
}

bool initialize() {
	objects[0] = {glm::vec3(-1.5f, 0, 0), glm::vec3(0), glm::vec3(0.75f), glm::vec4(0.25f, 0.75f, 1, 1), glm::vec3(1), 32, 0};
	objects[1] = {glm::vec3(0), glm::vec3(0), glm::vec3(0.75f), glm::vec4(1, 0.5f, 0.25f, 1), glm::vec3(1), 32, 2 * glm::pi<float>() / 3};
	objects[2] = {glm::vec3(1.5f, 0, 0), glm::vec3(0), glm::vec3(0.75f), glm::vec4(0.5f, 1, 0.5f, 1), glm::vec3(1), 32, 4 * glm::pi<float>() / 3};
	point_lights = {{{-2, 2, 2}, {0.5f, 0.7f, 1.0f}, 22.0f}};
	spot_lights = {{{2, 3, 2}, {-0.3f, -1.0f, -0.6f}, {1.0f, 0.55f, 0.25f}, 35.0f, 18.0f, 30.0f}};

	if (!graphics::core::initialize()) return false;
	std::vector<graphics::core::Vertex> vertices;
	std::vector<uint32_t> indices;
	createSphere(vertices, indices);
	if (!graphics::core::getGeometry(vertices, indices)) {
		graphics::core::shutdown();
		return false;
	}
	createPlane(vertices, indices);
	if (!graphics::core::getEnvironmentGeometry(vertices, indices)) {
		graphics::core::shutdown();
		return false;
	}
	return true;
}

void shutdown() { graphics::core::shutdown(); }

void update(double time, GLFWwindow* window) {
	const double delta = std::clamp(time - previous_time, 0.0, 0.1);
	previous_time = time;
	updateCameraFromKeyboard(window, static_cast<float>(delta));
	if (animation_playing) animation_time += delta;

	ImGui::Begin("Lab 2 - Camera and lighting");
	if (ImGui::CollapsingHeader("Projection and animation", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::RadioButton("Perspective projection", use_perspective)) use_perspective = true;
		if (ImGui::RadioButton("Orthographic projection", !use_perspective)) use_perspective = false;
		ImGui::Checkbox("Animation", &animation_playing);
		ImGui::SliderFloat("Move speed", &animation_speed, 0, 3);
		ImGui::SliderFloat("Orbit radius", &orbit_radius, 0, 1.5f);
		ImGui::SliderFloat("Rotation speed", &rotation_speed, 0, 3);
	}
	if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Movement speed", &camera.movement_speed, 0.5f, 12.0f);
		ImGui::SliderFloat("Turn speed", &camera.turn_speed, 20.0f, 180.0f);
		ImGui::SliderFloat("Field of view", &camera.field_of_view, 20.0f, 110.0f);
	}
	drawLightControls();
	if (ImGui::CollapsingHeader("Model material and transform", ImGuiTreeNodeFlags_DefaultOpen)) {
		const char* names[] = {"Sphere 1", "Sphere 2", "Sphere 3"};
		ImGui::Combo("Object", &selected_object, names, graphics::core::kMaxObjectCount);
		ObjectState& object = objects[static_cast<size_t>(selected_object)];
		ImGui::DragFloat3("Center", glm::value_ptr(object.position), 0.01f, -4, 4);
		ImGui::SliderAngle("Rotate X", &object.rotation.x, -180, 180);
		ImGui::SliderAngle("Rotate Y", &object.rotation.y, -180, 180);
		ImGui::SliderAngle("Rotate Z", &object.rotation.z, -180, 180);
		ImGui::DragFloat3("Scale", glm::value_ptr(object.scale), 0.01f, 0.1f, 3);
		ImGui::ColorEdit3("Albedo", glm::value_ptr(object.color));
		ImGui::ColorEdit3("Specular color", glm::value_ptr(object.specular));
		ImGui::SliderFloat("Shininess", &object.shininess, 1.0f, 256.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
	}
	ImGui::Checkbox("Show light markers", &show_light_markers);
	ImGui::End();
}

void render(const graphics::internal::FrameData& fd) {
	const auto [width, height] = graphics::internal::context.swapchain_extent;
	if (height == 0) return;

	graphics::core::SceneUniforms scene{};
	const glm::mat4 view_projection = makeProjectionMatrix(static_cast<float>(width) / height) * makeViewMatrix();
	copyMatrix(view_projection, scene.view_projection);
	const glm::vec3 eye = camera.position;
	scene.camera_position[0] = eye.x; scene.camera_position[1] = eye.y; scene.camera_position[2] = eye.z; scene.camera_position[3] = 1;
	scene.ambient_color_intensity[0] = ambient_color.r; scene.ambient_color_intensity[1] = ambient_color.g;
	scene.ambient_color_intensity[2] = ambient_color.b; scene.ambient_color_intensity[3] = ambient_intensity;
	const glm::vec3 dir = safeNormalize(directional_direction, {0, -1, 0});
	scene.directional_direction[0] = dir.x; scene.directional_direction[1] = dir.y; scene.directional_direction[2] = dir.z;
	scene.directional_color_intensity[0] = directional_color.r; scene.directional_color_intensity[1] = directional_color.g;
	scene.directional_color_intensity[2] = directional_color.b; scene.directional_color_intensity[3] = directional_intensity;
	scene.light_counts[0] = static_cast<uint32_t>(point_lights.size());
	scene.light_counts[1] = static_cast<uint32_t>(spot_lights.size());

	std::vector<graphics::core::ModelUniforms> model_uniforms;
	model_uniforms.reserve(objects.size() + 3 + (show_light_markers
		? point_lights.size() + spot_lights.size()
		: 0));

	const auto add_environment_surface = [&model_uniforms](const glm::vec3& position,
													 const glm::vec3& rotation,
													 const glm::vec3& scale,
													 const glm::vec3& color) {
		graphics::core::ModelUniforms surface{};
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position);
		transform = glm::rotate(transform, rotation.x, {1, 0, 0});
		transform = glm::rotate(transform, rotation.y, {0, 1, 0});
		transform = glm::rotate(transform, rotation.z, {0, 0, 1});
		transform = glm::scale(transform, scale);
		copyMatrix(transform, surface.model);
		surface.albedo[0] = color.r;
		surface.albedo[1] = color.g;
		surface.albedo[2] = color.b;
		surface.albedo[3] = 1.0f;
		surface.specular[0] = 0.18f;
		surface.specular[1] = 0.18f;
		surface.specular[2] = 0.18f;
		surface.material_parameters[0] = 24.0f;
		surface.material_parameters[2] = 1.0f;
		surface.material_parameters[3] = 1.0f;
		model_uniforms.push_back(surface);
	};
	add_environment_surface({0.0f, -1.15f, 0.0f}, {0, 0, 0}, {8.0f, 1.0f, 8.0f}, {0.14f, 0.17f, 0.23f});
	add_environment_surface({0.0f, 1.4f, -3.2f}, {glm::half_pi<float>(), 0, 0},
		{9.0f, 1.0f, 3.5f}, {0.075f, 0.085f, 0.14f});
	add_environment_surface({-8.0f, 2.3f, 0.0f}, {0, 0, -glm::half_pi<float>()},
		{4.0f, 1.0f, 8.0f}, {0.065f, 0.075f, 0.12f});

	for (const ObjectState& object : objects) {
		graphics::core::ModelUniforms model{};
		copyMatrix(makeModelMatrix(object, animation_time), model.model);
		model.albedo[0] = object.color.r; model.albedo[1] = object.color.g; model.albedo[2] = object.color.b; model.albedo[3] = object.color.a;
		model.specular[0] = object.specular.r; model.specular[1] = object.specular.g; model.specular[2] = object.specular.b;
		model.material_parameters[0] = object.shininess;
		model_uniforms.push_back(model);
	}
	std::vector<graphics::core::Vertex> visualization_lines;
	if (show_light_markers) {
		visualization_lines.reserve(spot_lights.size() * 80);
		const auto add_marker = [&model_uniforms](const glm::vec3& position, const glm::vec3& color, float radius = 0.12f) {
			graphics::core::ModelUniforms marker{};
			glm::mat4 transform = glm::translate(glm::mat4(1), position);
			transform = glm::scale(transform, glm::vec3(radius));
			copyMatrix(transform, marker.model);
			marker.albedo[0] = color.r; marker.albedo[1] = color.g; marker.albedo[2] = color.b; marker.albedo[3] = 1;
			marker.material_parameters[1] = 1.0f;
			model_uniforms.push_back(marker);
		};
		const auto add_line = [&visualization_lines](const glm::vec3& a, const glm::vec3& b) {
			visualization_lines.push_back({.position = {a.x, a.y, a.z}, .normal = {0, 1, 0}, .color = {1, 1, 1}});
			visualization_lines.push_back({.position = {b.x, b.y, b.z}, .normal = {0, 1, 0}, .color = {1, 1, 1}});
		};
		for (const auto& light : point_lights) add_marker(light.position, light.color);
		for (const auto& light : spot_lights) {
			add_marker(light.position, light.color);
			const glm::vec3 direction = safeNormalize(light.direction, {0, -1, 0});
			const glm::vec3 reference = std::abs(direction.y) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
			const glm::vec3 right = safeNormalize(glm::cross(direction, reference), {1, 0, 0});
			const glm::vec3 up = glm::cross(right, direction);
			const float angle = glm::radians(light.outer_angle);
			constexpr int ray_count = 8;
			constexpr int ring_segments = 32;
			constexpr float cone_length = 3.0f;
			const float cone_radius = cone_length * std::tan(angle);
			for (int ray = 0; ray < ray_count; ++ray) {
				const float around = 2.0f * glm::pi<float>() * ray / ray_count;
				const glm::vec3 radial = right * std::cos(around) + up * std::sin(around);
				const glm::vec3 endpoint = light.position + direction * cone_length + radial * cone_radius;
				add_line(light.position, endpoint);
			}
			for (int segment = 0; segment < ring_segments; ++segment) {
				const float a0 = 2.0f * glm::pi<float>() * segment / ring_segments;
				const float a1 = 2.0f * glm::pi<float>() * (segment + 1) / ring_segments;
				const glm::vec3 p0 = light.position + direction * cone_length +
					cone_radius * (right * std::cos(a0) + up * std::sin(a0));
				const glm::vec3 p1 = light.position + direction * cone_length +
					cone_radius * (right * std::cos(a1) + up * std::sin(a1));
				add_line(p0, p1);
			}
		}
	}

	std::vector<graphics::core::PointLight> gpu_points;
	gpu_points.reserve(point_lights.size());
	for (const auto& light : point_lights) {
		gpu_points.push_back({{light.position.x, light.position.y, light.position.z, 1},
			{light.color.r, light.color.g, light.color.b, light.intensity}});
	}
	std::vector<graphics::core::SpotLight> gpu_spots;
	gpu_spots.reserve(spot_lights.size());
	for (const auto& light : spot_lights) {
		const glm::vec3 direction = safeNormalize(light.direction, {0, -1, 0});
		gpu_spots.push_back({{light.position.x, light.position.y, light.position.z, 1},
			{direction.x, direction.y, direction.z, 0},
			{light.color.r, light.color.g, light.color.b, light.intensity},
			{std::cos(glm::radians(light.inner_angle)), std::cos(glm::radians(light.outer_angle)), 0, 0}});
	}

	graphics::core::render(fd, scene, model_uniforms, gpu_points, gpu_spots, visualization_lines);
}

}
