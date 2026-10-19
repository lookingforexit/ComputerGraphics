#ifndef UNIFORMS_GLSL
#define UNIFORMS_GLSL

layout(std140, set = 0, binding = 0) uniform SceneUniforms {
    mat4 view_projection;
    vec3 camera_position;
    float camera_padding;
    vec3 ambient_color;
    float ambient_intensity;
    vec3 directional_direction;
    float direction_padding;
    vec3 directional_color;
    float directional_intensity;
    uint point_light_count;
    uint spot_light_count;
    uvec2 count_padding;
} scene;

layout(std140, set = 1, binding = 0) uniform ObjectUniforms {
    mat4 model;
    vec4 albedo;
    vec3 specular_color;
    float shininess;
    uint use_vertex_color;
    uint material_padding_0;
    uint material_padding_1;
    uint material_padding_2;
} object_data;

#endif
