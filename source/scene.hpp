#pragma once

#include <array>
#include "camera.hpp"
#include "render_data.hpp"

namespace application {

struct Transform {
    glm::vec3 position{};
    glm::vec3 rotation_radians{};
    glm::vec3 scale{1.0f};

    glm::mat4 matrix() const;
};

struct SceneObject {
    const char *name = "Object";
    graphics::core::MeshKind mesh = graphics::core::MeshKind::Sphere;
    Transform transform;
    graphics::core::Material material;
    float orbit_phase_radians = 0.0f;
};

struct Animation {
    bool playing = true;
    double time = 0.0;
    float orbit_speed = 1.0f;
    float orbit_radius = 0.35f;
    float rotation_speed = 0.6f;
};

struct Scene {
    std::array<SceneObject, 3> objects;
    std::array<SceneObject, 3> room;
    graphics::core::AmbientLight ambient;
    graphics::core::DirectionalLight directional;
    std::vector<graphics::core::PointLight> point_lights;
    std::vector<graphics::core::SpotLight> spot_lights;
    Animation animation;
    bool show_light_markers = true;

    graphics::core::RenderFrame make_render_frame(const Camera &camera, float aspect) const;
};

Scene make_default_scene();

}
