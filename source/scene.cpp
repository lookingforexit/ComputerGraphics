#include "scene.hpp"
#include "geometry.hpp"

#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace application {
namespace {

graphics::core::RenderObject make_light_marker(const glm::vec3 &position,
                                               const glm::vec3 &color) {
    constexpr float marker_radius = 0.12f;
    const Transform transform{
        .position = position,
        .scale = glm::vec3(marker_radius),
    };
    return {
        .mesh = graphics::core::MeshKind::Sphere,
        .shading = graphics::core::Shading::Unlit,
        .model = transform.matrix(),
        .material = {.albedo = glm::vec4(color, 1.0f)},
    };
}

Transform animate_object(const SceneObject &object, const Animation &animation) {
    Transform transform = object.transform;
    const float time = static_cast<float>(animation.time);
    const float orbit_angle = time * animation.orbit_speed + object.orbit_phase_radians;
    transform.position.x += animation.orbit_radius * std::cos(orbit_angle);
    transform.position.y += animation.orbit_radius * std::sin(orbit_angle);
    transform.rotation_radians.x += time * animation.rotation_speed;
    transform.rotation_radians.y += time * animation.rotation_speed;
    return transform;
}

}

glm::mat4 Transform::matrix() const {
    glm::mat4 model = glm::translate(glm::mat4(1.0f), position);
    model = glm::rotate(model, rotation_radians.x, {1, 0, 0});
    model = glm::rotate(model, rotation_radians.y, {0, 1, 0});
    model = glm::rotate(model, rotation_radians.z, {0, 0, 1});
    return glm::scale(model, scale);
}

Scene make_default_scene() {
    Scene scene;

    SceneObject blue_sphere{
        .name = "Blue sphere",
        .transform = {
            .position = {-1.5f, 0.0f, 0.0f},
            .scale = glm::vec3(0.75f),
        },
        .material = {
            .albedo = {0.25f, 0.75f, 1.0f, 1.0f},
            .specular_color = glm::vec3(1.0f),
            .shininess = 32.0f,
            .use_vertex_color = true,
        },
        .orbit_phase_radians = 0.0f,
    };

    SceneObject orange_sphere = blue_sphere;
    orange_sphere.name = "Orange sphere";
    orange_sphere.transform.position = {0.0f, 0.0f, 0.0f};
    orange_sphere.material.albedo = {1.0f, 0.5f, 0.25f, 1.0f};
    orange_sphere.orbit_phase_radians = glm::two_pi<float>() / 3.0f;

    SceneObject green_sphere = blue_sphere;
    green_sphere.name = "Green sphere";
    green_sphere.transform.position = {1.5f, 0.0f, 0.0f};
    green_sphere.material.albedo = {0.5f, 1.0f, 0.5f, 1.0f};
    green_sphere.orbit_phase_radians = 2.0f * glm::two_pi<float>() / 3.0f;

    scene.objects = {blue_sphere, orange_sphere, green_sphere};

    const graphics::core::Material room_material{
        .albedo = {0.14f, 0.17f, 0.23f, 1.0f},
        .specular_color = glm::vec3(0.18f),
        .shininess = 24.0f,
        .use_vertex_color = false,
    };

    SceneObject floor{
        .name = "Floor",
        .mesh = graphics::core::MeshKind::Plane,
        .transform = {
            .position = {0.0f, -1.15f, 0.0f},
            .scale = {8.0f, 1.0f, 8.0f},
        },
        .material = room_material,
    };

    SceneObject back_wall{
        .name = "Back wall",
        .mesh = graphics::core::MeshKind::Plane,
        .transform = {
            .position = {0.0f, 1.4f, -3.2f},
            .rotation_radians = {glm::half_pi<float>(), 0.0f, 0.0f},
            .scale = {9.0f, 1.0f, 3.5f},
        },
        .material = room_material,
    };
    back_wall.material.albedo = {0.075f, 0.085f, 0.14f, 1.0f};

    SceneObject side_wall{
        .name = "Side wall",
        .mesh = graphics::core::MeshKind::Plane,
        .transform = {
            .position = {-8.0f, 2.3f, 0.0f},
            .rotation_radians = {0.0f, 0.0f, -glm::half_pi<float>()},
            .scale = {4.0f, 1.0f, 8.0f},
        },
        .material = room_material,
    };
    side_wall.material.albedo = {0.065f, 0.075f, 0.12f, 1.0f};

    scene.room = {floor, back_wall, side_wall};

    scene.point_lights.push_back({
        .position = {-2.0f, 2.0f, 2.0f},
        .color = {0.5f, 0.7f, 1.0f},
        .intensity = 22.0f,
    });
    scene.spot_lights.push_back({
        .position = {2.0f, 3.0f, 2.0f},
        .direction = {-0.3f, -1.0f, -0.6f},
        .color = {1.0f, 0.55f, 0.25f},
        .intensity = 35.0f,
        .inner_angle_degrees = 18.0f,
        .outer_angle_degrees = 30.0f,
    });
    return scene;
}

graphics::core::RenderFrame Scene::make_render_frame(const Camera &camera, float aspect) const {
    graphics::core::RenderFrame frame;
    frame.view_projection = camera.projection_matrix(aspect) * camera.view_matrix();
    frame.camera_position = camera.position;
    frame.ambient = ambient;
    frame.directional = directional;
    frame.point_lights = point_lights;
    frame.spot_lights = spot_lights;

    const size_t marker_count = show_light_markers ? point_lights.size() + spot_lights.size() : 0;
    frame.objects.reserve(room.size() + objects.size() + marker_count);

    for (const SceneObject &surface : room) {
        frame.objects.push_back({
            .mesh = surface.mesh,
            .shading = graphics::core::Shading::Lit,
            .model = surface.transform.matrix(),
            .material = surface.material,
        });
    }
    for (const SceneObject &object : objects) {
        frame.objects.push_back({
            .mesh = object.mesh,
            .shading = graphics::core::Shading::Lit,
            .model = animate_object(object, animation).matrix(),
            .material = object.material,
        });
    }

    if (show_light_markers) {
        for (const auto &light : point_lights) {
            frame.objects.push_back(make_light_marker(light.position, light.color));
        }
        for (const auto &light : spot_lights) {
            frame.objects.push_back(make_light_marker(light.position, light.color));
            geometry::append_spotlight_cone(frame.light_lines, light);
        }
    }
    return frame;
}

}
