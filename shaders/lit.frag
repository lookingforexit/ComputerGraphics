#version 450
#extension GL_GOOGLE_include_directive : require
#include "uniforms.glsl"
#include "lights.glsl"

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec3 vertex_color;
layout(location = 3) in vec2 texcoord;
layout(location = 0) out vec4 out_color;

layout(set = 1, binding = 1) uniform sampler2D albedo_texture;
layout(set = 1, binding = 2) uniform sampler2D specular_texture;
layout(set = 1, binding = 3) uniform sampler2D emissive_texture;

layout(push_constant) uniform SamplingConstants {
    uint animated;
    float time;
    float strength;
} sampling;

layout(set = 0, binding = 3) uniform sampler2DShadow sun_shadow;
layout(set = 0, binding = 4) uniform sampler2DArrayShadow spot_shadow;

float visibility(sampler2DShadow shadow_map, mat4 matrix, vec3 position) {
    vec4 clip = matrix * vec4(position, 1.0);
    if (clip.w <= 0.0) {
        return 1.0;
    }
    vec3 projected = clip.xyz / clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    return texture(shadow_map, vec3(uv, projected.z));
}

float spot_visibility(uint index, vec3 position) {
    vec4 clip = spot_buffer.lights[index].shadow_matrix * vec4(position, 1.0);
    if (clip.w <= 0.0) {
        return 1.0;
    }
    vec3 projected = clip.xyz / clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    return texture(spot_shadow, vec4(uv, float(index), projected.z));
}

struct TextureSamples {
    vec3 albedo;
    vec3 specular;
    vec3 emissive;
};

TextureSamples sample_material() {
    vec2 first_uv = texcoord;
    vec2 second_uv = texcoord;
    float blend = 0.0;
    if (sampling.animated != 0) {
        float time = sampling.time;
        float strength = sampling.strength;

        vec2 center = vec2(0.5, 0.5);
        vec2 offset = texcoord - center;
        float dist = length(offset);

        float angle = (1.0 - smoothstep(0.0, 0.5, dist)) * strength * 10.0 + time * 2.0;

        float s = sin(angle);
        float c = cos(angle);
        first_uv = center + vec2(offset.x * c - offset.y * s, offset.x * s + offset.y * c);

        float s2 = sin(-angle * 0.7);
        float c2 = cos(-angle * 0.7);
        second_uv = center + vec2(offset.x * c2 - offset.y * s2, offset.x * s2 + offset.y * c2) * 1.3;

        blend = 0.5;
    }
    TextureSamples result;
    result.albedo = mix(texture(albedo_texture, first_uv).rgb,
                        texture(albedo_texture, second_uv).rgb, blend);
    result.specular = mix(texture(specular_texture, first_uv).rgb,
                          texture(specular_texture, second_uv).rgb, blend);
    result.emissive = mix(texture(emissive_texture, first_uv).rgb,
                          texture(emissive_texture, second_uv).rgb, blend);
    return result;
}

const float MIN_LIGHT_DISTANCE_SQUARED = 0.0001;

struct Surface {
    vec3 position;
    vec3 normal;
    vec3 view_direction;
    vec3 color;
    vec3 specular;
};

vec3 blinn_phong(Surface surface, vec3 light_direction, vec3 radiance) {
    float diffuse_factor = max(dot(surface.normal, light_direction), 0.0);
    vec3 half_direction = light_direction + surface.view_direction;
    float half_length_squared = max(dot(half_direction, half_direction), 0.000001);
    half_direction *= inversesqrt(half_length_squared);
    float specular_factor = pow(max(dot(surface.normal, half_direction), 0.0),
                                max(object_data.shininess, 1.0));
    vec3 diffuse = surface.color * diffuse_factor;
    vec3 specular = surface.specular * specular_factor * step(0.000001, diffuse_factor);
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

    bool receives_shadows = dot(surface.normal, surface.view_direction) > 0.0;
    
    TextureSamples texels = sample_material();
    surface.color = object_data.albedo.rgb * texels.albedo;
    surface.specular = object_data.specular_color * texels.specular;
    if (object_data.use_vertex_color != 0) {
        surface.color *= vertex_color;
    }

    vec3 illumination = surface.color * scene.ambient_color * scene.ambient_intensity;
    illumination += directional_lighting(surface) *
        (receives_shadows ? visibility(sun_shadow, scene.sun_shadow_matrix, surface.position) : 1.0);
    for (uint i = 0; i < scene.point_light_count; ++i) {
        illumination += point_lighting(surface, point_buffer.lights[i]);
    }
    for (uint i = 0; i < scene.spot_light_count; ++i) {
        illumination += spot_lighting(surface, spot_buffer.lights[i]) *
            (receives_shadows ? spot_visibility(i, surface.position) : 1.0);
    }
    out_color = vec4(illumination + texels.emissive, object_data.albedo.a);
}
