#include "application.hpp"

#include <cmath>
#include <algorithm>
#include <array>
#include <cstring>
#include <imgui.h>
#include <vector>

#include "graphics.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"
#include "glm/ext/matrix_clip_space.hpp"
#include "glm/ext/matrix_transform.hpp"
#include "glm/ext/scalar_constants.hpp"
#include "glm/gtc/type_ptr.hpp"

namespace application {
	namespace {
		// 10 * (12 - 1) = 110
		constexpr uint32_t longitude = 12;
		constexpr uint32_t latitude = 10;

		struct ObjectState {
			glm::vec3 position;
			glm::vec3 rotation;
			glm::vec3 scale;
			glm::vec4 color;
			float phase;
		};

		std::array<ObjectState, graphics::core::kMaxObjectCount> objects;

		bool use_perspective = true;
		bool animation_playing = true;

		float animation_speed = 1;
		float orbit_radius = 0.35;
		float rotation_speed = 0.6;

		int selected_object = 0;

		double previous_time = 0;
		double animation_time = 0;

		void createSphere(std::vector<graphics::core::Vertex>& vertices, std::vector<uint32_t>& indices)
        {
            vertices.clear();
            indices.clear();

		    vertices.push_back({
                .position = {0, 1, 0},
                .color = {0.5, 1, 0.5}
            });

            for (uint32_t lat = 1; lat < latitude; ++lat) {
                const float theta = glm::pi<float>() * static_cast<float>(lat) / static_cast<float>(latitude);

                for (uint32_t lon = 0; lon < longitude; ++lon) {
                    const float phi = 2 * glm::pi<float>() * static_cast<float>(lon) / static_cast<float>(longitude);

                    const float x = std::sin(theta) * std::cos(phi);
                    const float y = std::cos(theta);
                    const float z = std::sin(theta) * std::sin(phi);

                    vertices.push_back({
                        .position = { x, y, z },
                        .color = { 0.5f + 0.5f * x, 0.5f + 0.5f * y, 0.5f + 0.5f * z }
                    });
                }
            }

            const auto bottom = static_cast<uint32_t>(vertices.size());

            vertices.push_back({
	            .position = {0, -1, 0},
	            .color = {0.5, 0, 0.5}
            });

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

                    indices.insert(indices.end(), { a, b, c, b, d, c });
                }
            }

            const uint32_t last_ring = bottom - longitude;

            for (uint32_t lon = 0; lon < longitude; ++lon) {
                const uint32_t next = (lon + 1) % longitude;

                indices.insert(indices.end(), {bottom, last_ring + lon, last_ring + next});
            }
        }

		void copyMatrix(const glm::mat4& source, float destination[4][4])
		{
		    std::memcpy(destination,glm::value_ptr(source),sizeof(float) * 16);
		}

		glm::mat4 makeModelMatrix(const ObjectState& object, double time)
		{
		    const auto t = static_cast<float>(time);

		    glm::vec3 animated_position = object.position;
		    animated_position.x += orbit_radius * std::cos(t * animation_speed + object.phase);
		    animated_position.y += orbit_radius * std::sin(t * animation_speed + object.phase);

		    glm::mat4 model(1);

		    model = glm::translate(model, animated_position);

		    model = glm::rotate(model,object.rotation.x + t * rotation_speed,glm::vec3(1, 0, 0));
		    model = glm::rotate(model,object.rotation.y + t * rotation_speed,glm::vec3(0, 1, 0));
		    model = glm::rotate(model, object.rotation.z,glm::vec3(0, 0, 1));

		    model = glm::scale(model, object.scale);

		    return model;
		}

		glm::mat4 makeProjectionMatrix(float aspect) {
		    glm::mat4 projection(1);

		    if (use_perspective) {
		        projection = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
		    } else {
		        const float half_height = 2.5;
		        const float half_width = half_height * aspect;

		        projection = glm::ortho(-half_width, half_width, -half_height, half_height,0.1f,100.0f);
		    }

		    projection[1][1] *= -1;

		    return projection;
		}
	}


bool initialize() {
		objects[0].position = {-1.5, 0, 0};
		objects[0].rotation = {0, 0, 0};
		objects[0].scale = {0.75f, 0.75f, 0.75f};
		objects[0].color = {0.25, 0.75, 1, 1};
		objects[0].phase = 0;

		objects[1].position = {0, 0, 0};
		objects[1].rotation = {0, 0, 0};
		objects[1].scale = {0.75f, 0.75f, 0.75f};
		objects[1].color = {1, 0.5, 0.25, 1};
		objects[1].phase = 2 * glm::pi<float>() / 3;

		objects[2].position = {1.5, 0, 0};
		objects[2].rotation = {0, 0, 0};
		objects[2].scale = {0.75f, 0.75f, 0.75f};
		objects[2].color = {0.5, 1, 0.5, 1};
		objects[2].phase = 4 * glm::pi<float>() / 3;

		if (!graphics::core::initialize()) {
			return false;
		}

		std::vector<graphics::core::Vertex> vertices;
		std::vector<uint32_t> indices;

		createSphere(vertices, indices);

		// 10 * (12 - 1) = 110 vertices
		// 6 * 12 * (10 - 1) = 648 indices
		if (vertices.size() != 110 || indices.size() != 648) {
			graphics::core::shutdown();
			return false;
		}

		if (!graphics::core::getGeometry(vertices, indices)) {
			graphics::core::shutdown();
			return false;
		}

		return true;
}

void shutdown() {
	graphics::core::shutdown();
}

void update([[maybe_unused]] double time) {
	const double delta = std::clamp(time - previous_time, 0.0, 0.1);
	previous_time = time;

	if (animation_playing) {
		animation_time += delta;
	}

	ImGui::Begin("Lab1");

	ImGui::Text("Sphere");

	if (ImGui::RadioButton("Perspective projection", use_perspective)) {
		use_perspective = true;
	}

	if (ImGui::RadioButton("Orthographic projection", !use_perspective)) {
		use_perspective = false;
	}

	ImGui::Checkbox("Animation", &animation_playing);
	ImGui::SliderFloat("Move speed", &animation_speed, 0, 3);
	ImGui::SliderFloat("Radius", &orbit_radius, 0, 1.5);
	ImGui::SliderFloat("Rotation speed", &rotation_speed, 0, 3);

	const char* object_names[] = {"Sphere 1", "Sphere 2", "Sphere 3"};

	ImGui::Combo("Object", &selected_object, object_names,  graphics::core::kMaxObjectCount);

	ObjectState& object = objects[selected_object];

	ImGui::DragFloat3("Center", glm::value_ptr(object.position), 0.01, -4, 4);

	ImGui::SliderAngle("Rotate X", &object.rotation.x,-180, 180);
	ImGui::SliderAngle("Rotate Y", &object.rotation.y,-180,180);
	ImGui::SliderAngle("Rotate Z", &object.rotation.z,-180,180);

	ImGui::DragFloat3("Scale",glm::value_ptr(object.scale),0.01,0.1,3);

	ImGui::ColorEdit3("Color",glm::value_ptr(object.color));

	ImGui::End();
}

void render(const graphics::internal::FrameData& fd) {
	const auto&[width, height] = graphics::internal::context.swapchain_extent;

	if (height == 0) {
		return;
	}

	const float aspect = static_cast<float>(width) / static_cast<float>(height);

	graphics::core::SceneUniforms scene{};

	constexpr glm::mat4 view = glm::translate(glm::mat4(1), glm::vec3(0, 0, -7));

	const glm::mat4 projection = makeProjectionMatrix(aspect);

	copyMatrix(projection, scene.projection);
	copyMatrix(view, scene.view);

	std::vector<graphics::core::ModelUniforms> model_uniforms;
	model_uniforms.reserve(objects.size());

	for (const ObjectState& object : objects) {
		graphics::core::ModelUniforms model_data{};

		const glm::mat4 model = makeModelMatrix(object, animation_time);

		copyMatrix(model, model_data.model);

		model_data.color[0] = object.color.r;
		model_data.color[1] = object.color.g;
		model_data.color[2] = object.color.b;
		model_data.color[3] = object.color.a;

		model_uniforms.push_back(model_data);
	}

	graphics::core::render(fd, scene, model_uniforms);
}

} // namespace application
