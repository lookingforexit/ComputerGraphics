#include "application.hpp"

#include <algorithm>
#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include "geometry.hpp"
#include "graphics.hpp"
#include "scene.hpp"

namespace application {
namespace {

Scene scene;
Camera camera;
int selected_object = 0;
double previous_time = 0.0;

void draw_light_controls() {
    if (ImGui::CollapsingHeader("Ambient light", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3("Ambient color", glm::value_ptr(scene.ambient.color));
        ImGui::SliderFloat("Ambient intensity", &scene.ambient.intensity, 0.0f, 2.0f);
    }
    if (ImGui::CollapsingHeader("Directional light", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Direction", glm::value_ptr(scene.directional.direction), 0.01f, -1.0f, 1.0f);
        ImGui::ColorEdit3("Directional color", glm::value_ptr(scene.directional.color));
        ImGui::SliderFloat("Directional intensity", &scene.directional.intensity, 0.0f, 5.0f);
    }
    if (ImGui::CollapsingHeader("Point lights", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Add point light")) {
            scene.point_lights.emplace_back();
        }
        for (size_t i = 0; i < scene.point_lights.size(); ++i) {
            auto &light = scene.point_lights[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode("point", "Point light %zu", i + 1)) {
                ImGui::DragFloat3("Position", glm::value_ptr(light.position), 0.05f);
                ImGui::ColorEdit3("Color", glm::value_ptr(light.color));
                ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 100.0f);
                if (ImGui::Button("Remove point light")) {
                    scene.point_lights.erase(scene.point_lights.begin() + static_cast<std::ptrdiff_t>(i));
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Spot lights", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Add spot light")) {
            scene.spot_lights.emplace_back();
        }
        for (size_t i = 0; i < scene.spot_lights.size(); ++i) {
            auto &light = scene.spot_lights[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode("spot", "Spot light %zu", i + 1)) {
                ImGui::DragFloat3("Position", glm::value_ptr(light.position), 0.05f);
                ImGui::DragFloat3("Direction", glm::value_ptr(light.direction), 0.01f, -1, 1);
                ImGui::ColorEdit3("Color", glm::value_ptr(light.color));
                ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 100.0f);
                ImGui::SliderFloat("Inner angle", &light.inner_angle_degrees, 0.0f, light.outer_angle_degrees);
                ImGui::SliderFloat("Outer angle", &light.outer_angle_degrees, light.inner_angle_degrees, 89.0f);
                if (ImGui::Button("Remove spot light")) {
                    scene.spot_lights.erase(scene.spot_lights.begin() + static_cast<std::ptrdiff_t>(i));
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
}

void draw_animation_controls() {
    if (!ImGui::CollapsingHeader("Projection and animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    if (ImGui::RadioButton("Perspective projection", camera.use_perspective)) {
        camera.use_perspective = true;
    }
    if (ImGui::RadioButton("Orthographic projection", !camera.use_perspective)) {
        camera.use_perspective = false;
    }
    ImGui::Checkbox("Animation", &scene.animation.playing);
    ImGui::SliderFloat("Move speed", &scene.animation.orbit_speed, 0, 3);
    ImGui::SliderFloat("Orbit radius", &scene.animation.orbit_radius, 0, 1.5f);
    ImGui::SliderFloat("Rotation speed", &scene.animation.rotation_speed, 0, 3);
}

void draw_camera_controls() {
    if (!ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::SliderFloat("Movement speed", &camera.movement_speed, 0.5f, 12.0f);
    ImGui::SliderFloat("Turn speed", &camera.turn_speed_degrees, 20.0f, 180.0f);
    ImGui::SliderFloat("Field of view", &camera.field_of_view_degrees, 20.0f, 110.0f);
}

void draw_object_controls() {
    if (!ImGui::CollapsingHeader("Model material and transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    if (ImGui::BeginCombo("Object", scene.objects[selected_object].name)) {
        for (size_t i = 0; i < scene.objects.size(); ++i) {
            if (ImGui::Selectable(scene.objects[i].name, selected_object == static_cast<int>(i))) {
                selected_object = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    SceneObject &object = scene.objects[static_cast<size_t>(selected_object)];
    ImGui::DragFloat3("Center", glm::value_ptr(object.transform.position), 0.01f, -4, 4);
    ImGui::SliderAngle("Rotate X", &object.transform.rotation_radians.x, -180, 180);
    ImGui::SliderAngle("Rotate Y", &object.transform.rotation_radians.y, -180, 180);
    ImGui::SliderAngle("Rotate Z", &object.transform.rotation_radians.z, -180, 180);
    ImGui::DragFloat3("Scale", glm::value_ptr(object.transform.scale), 0.01f, 0.1f, 3);
    ImGui::ColorEdit3("Albedo", glm::value_ptr(object.material.albedo));
    ImGui::ColorEdit3("Specular color", glm::value_ptr(object.material.specular_color));
    ImGui::SliderFloat("Shininess", &object.material.shininess, 1.0f, 256.0f, "%.0f",
                       ImGuiSliderFlags_Logarithmic);
}

void draw_control_panel() {
    ImGui::Begin("Lab 2 - Camera and lighting");
    draw_animation_controls();
    draw_camera_controls();
    draw_light_controls();
    draw_object_controls();
    ImGui::Checkbox("Show light markers", &scene.show_light_markers);
    ImGui::End();
}

}

bool initialize() {
    scene = make_default_scene();
    camera = Camera{};
    selected_object = 0;
    previous_time = 0.0;

    if (!graphics::core::initialize()) {
        return false;
    }

    constexpr uint32_t sphere_longitude_segments = 120;
    constexpr uint32_t sphere_latitude_segments = 100;
    const auto sphere = geometry::make_sphere(sphere_longitude_segments, sphere_latitude_segments);
    const auto plane = geometry::make_plane();
    if (!graphics::core::upload_mesh(graphics::core::MeshKind::Sphere, sphere) ||
        !graphics::core::upload_mesh(graphics::core::MeshKind::Plane, plane)) {
        graphics::core::shutdown();
        return false;
    }
    return true;
}

void shutdown() {
    graphics::core::shutdown();
}

void update(double time, GLFWwindow *window) {
    constexpr double maximum_frame_delta = 0.1;
    const double delta_time = std::clamp(time - previous_time, 0.0, maximum_frame_delta);
    previous_time = time;

    camera.update_from_keyboard(window, static_cast<float>(delta_time));
    if (scene.animation.playing) {
        scene.animation.time += delta_time;
    }
    draw_control_panel();
}

void render(const graphics::internal::FrameData &fd) {
    const auto [width, height] = graphics::internal::context.swapchain_extent;
    if (width == 0 || height == 0) {
        return;
    }
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const auto frame = scene.make_render_frame(camera, aspect);
    graphics::core::render(fd, frame);
}

}