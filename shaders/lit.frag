#version 450
#extension GL_GOOGLE_include_directive : require
#include "uniforms.glsl"
#include "lights.glsl"

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec3 vertex_color;
layout(location = 0) out vec4 out_color;

const float MIN_LIGHT_DISTANCE_SQUARED = 0.0001;

struct Surface {
    vec3 position;
    vec3 normal;
    vec3 view_direction;
    vec3 color;
};

vec3 blinn_phong(Surface surface, vec3 light_direction, vec3 radiance) {
    float diffuse_factor = max(dot(surface.normal, light_direction), 0.0);
    vec3 half_direction = light_direction + surface.view_direction;
    float half_length_squared = max(dot(half_direction, half_direction), 0.000001);
    half_direction *= inversesqrt(half_length_squared);
    float specular_factor = pow(max(dot(surface.normal, half_direction), 0.0),
                                max(object_data.shininess, 1.0));
    vec3 diffuse = surface.color * diffuse_factor;
    vec3 specular = object_data.specular_color * specular_factor;
    return (diffuse + specular) * radiance;
}

vec3 directional_lighting(Surface surface) {
    vec3 to_light = -scene.directional_direction;
    vec3 radiance = scene.directional_color * scene.directional_intensity;
    return blinn_phong(surface, to_light, radiance);
}

vec3 point_lighting(Surface surface, PointLight light) {
    vec3 to_light = light.position - surface.position;
    float distance_squared = max(dot(to_light, to_light), MIN_LIGHT_DISTANCE_SQUARED);
    vec3 light_direction = to_light * inversesqrt(distance_squared);
    vec3 radiance = light.color * light.intensity / distance_squared;
    return blinn_phong(surface, light_direction, radiance);
}

float spotlight_falloff(SpotLight light, vec3 light_direction) {
    float cone_cosine = dot(light.direction, -light_direction);
    if (light.inner_cone_cosine <= light.outer_cone_cosine) {
        return step(light.outer_cone_cosine, cone_cosine);
    }
    return smoothstep(light.outer_cone_cosine, light.inner_cone_cosine, cone_cosine);
}

vec3 spot_lighting(Surface surface, SpotLight light) {
    vec3 to_light = light.position - surface.position;
    float distance_squared = max(dot(to_light, to_light), MIN_LIGHT_DISTANCE_SQUARED);
    vec3 light_direction = to_light * inversesqrt(distance_squared);
    float cone_factor = spotlight_falloff(light, light_direction);
    vec3 radiance = light.color * light.intensity * cone_factor / distance_squared;
    return blinn_phong(surface, light_direction, radiance);
}

void main() {
    Surface surface;
    surface.position = world_position;
    surface.normal = normalize(world_normal);
    surface.view_direction = normalize(scene.camera_position - world_position);
    surface.color = object_data.albedo.rgb;
    if (object_data.use_vertex_color != 0) {
        surface.color *= vertex_color;
    }

    vec3 illumination = surface.color * scene.ambient_color * scene.ambient_intensity;
    illumination += directional_lighting(surface);
    for (uint i = 0; i < scene.point_light_count; ++i) {
        illumination += point_lighting(surface, point_buffer.lights[i]);
    }
    for (uint i = 0; i < scene.spot_light_count; ++i) {
        illumination += spot_lighting(surface, spot_buffer.lights[i]);
    }
    out_color = vec4(illumination, object_data.albedo.a);
}
