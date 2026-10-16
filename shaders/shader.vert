#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec3 vertex_color;

layout(std140, set = 0, binding = 0) uniform SceneUniforms {
    mat4 view_projection;
    vec4 camera_position;
    vec4 ambient_color_intensity;
    vec4 directional_direction;
    vec4 directional_color_intensity;
    uvec4 light_counts;
} scene;

layout(std140, set = 1, binding = 0) uniform ModelUniforms {
    mat4 model;
    vec4 albedo;
    vec4 specular;
    vec4 material_parameters;
} object_data;

void main() {
    vec4 world = object_data.model * vec4(in_position, 1.0);
    world_position = world.xyz;
    world_normal = normalize(mat3(transpose(inverse(object_data.model))) * in_normal);
    vertex_color = in_color;
    gl_Position = scene.view_projection * world;
}
