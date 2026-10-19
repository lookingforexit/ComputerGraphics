#ifndef LIGHTS_GLSL
#define LIGHTS_GLSL

struct PointLight {
    vec3 position;
    float position_padding;
    vec3 color;
    float intensity;
};

struct SpotLight {
    vec3 position;
    float position_padding;
    vec3 direction;
    float direction_padding;
    vec3 color;
    float intensity;
    float inner_cone_cosine;
    float outer_cone_cosine;
    vec2 cone_padding;
};

layout(std430, set = 0, binding = 1) readonly buffer PointLightBuffer {
    PointLight lights[];
} point_buffer;

layout(std430, set = 0, binding = 2) readonly buffer SpotLightBuffer {
    SpotLight lights[];
} spot_buffer;

#endif
