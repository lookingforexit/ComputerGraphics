#version 450

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec3 vertex_color;
layout(location = 0) out vec4 out_color;

layout(std140, set = 0, binding = 0) uniform SceneUniforms {
    mat4 view_projection;
    vec4 camera_position;
    vec4 ambient_color_intensity;
    vec4 directional_direction;
    vec4 directional_color_intensity;
    uvec4 light_counts;
} scene;

struct PointLight {
    vec4 position;
    vec4 color_intensity;
};
struct SpotLight {
    vec4 position;
    vec4 direction;
    vec4 color_intensity;
    vec4 cone_angles;
};
layout(std430, set = 0, binding = 1) readonly buffer PointLightBuffer { PointLight lights[]; } point_buffer;
layout(std430, set = 0, binding = 2) readonly buffer SpotLightBuffer { SpotLight lights[]; } spot_buffer;

layout(std140, set = 1, binding = 0) uniform ModelUniforms {
    mat4 model;
    vec4 albedo;
    vec4 specular;
    vec4 material_parameters;
} object_data;

vec3 blinn_phong(vec3 normal, vec3 light_direction, vec3 radiance,
                 vec3 view_direction, vec3 base_color) {
    float diffuse_factor = max(dot(normal, light_direction), 0.0);
    vec3 half_direction = normalize(light_direction + view_direction);
    float specular_factor = pow(max(dot(normal, half_direction), 0.0),
                                max(object_data.material_parameters.x, 1.0));
    vec3 diffuse = base_color * diffuse_factor;
    vec3 specular = object_data.specular.rgb * specular_factor;
    return (diffuse + specular) * radiance;
}

void main() {
    if (object_data.material_parameters.y > 0.5) {
        out_color = vec4(object_data.albedo.rgb, 1.0);
        return;
    }

    vec3 base_color = object_data.albedo.rgb;
    if (object_data.material_parameters.z < 0.5) {
        base_color *= vertex_color;
    }
    vec3 normal = normalize(world_normal);
    vec3 view_direction = normalize(scene.camera_position.xyz - world_position);
    vec3 illumination = base_color * scene.ambient_color_intensity.rgb * scene.ambient_color_intensity.a;

    vec3 directional_l = normalize(-scene.directional_direction.xyz);
    vec3 directional_radiance = scene.directional_color_intensity.rgb * scene.directional_color_intensity.a;
    illumination += blinn_phong(normal, directional_l, directional_radiance, view_direction, base_color);

    for (uint i = 0; i < scene.light_counts.x; ++i) {
        PointLight light = point_buffer.lights[i];
        vec3 to_light = light.position.xyz - world_position;
        float distance_squared = max(dot(to_light, to_light), 0.0001);
        vec3 L = to_light * inversesqrt(distance_squared);
        vec3 radiance = light.color_intensity.rgb * (light.color_intensity.a / distance_squared);
        illumination += blinn_phong(normal, L, radiance, view_direction, base_color);
    }

    for (uint i = 0; i < scene.light_counts.y; ++i) {
        SpotLight light = spot_buffer.lights[i];
        vec3 to_light = light.position.xyz - world_position;
        float distance_squared = max(dot(to_light, to_light), 0.0001);
        vec3 L = to_light * inversesqrt(distance_squared);
        float cone_cosine = dot(normalize(light.direction.xyz), -L);
        float cone = smoothstep(light.cone_angles.y, light.cone_angles.x, cone_cosine);
        vec3 radiance = light.color_intensity.rgb * (light.color_intensity.a * cone / distance_squared);
        illumination += blinn_phong(normal, L, radiance, view_direction, base_color);
    }

    out_color = vec4(illumination, object_data.albedo.a);
}
