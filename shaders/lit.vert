#version 450
#extension GL_GOOGLE_include_directive : require
#include "uniforms.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec3 vertex_color;

void main() {
    vec4 position = object_data.model * vec4(in_position, 1.0);
    mat3 normal_matrix = transpose(inverse(mat3(object_data.model)));
    world_position = position.xyz;
    world_normal = normalize(normal_matrix * in_normal);
    vertex_color = in_color;
    gl_Position = scene.view_projection * position;
}
