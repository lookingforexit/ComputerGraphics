#version 450
#extension GL_GOOGLE_include_directive : require
#include "uniforms.glsl"
#include "lights.glsl"

layout(location = 0) in vec3 position;
layout(push_constant) uniform ShadowConstants {
    layout(offset = 16) int index;
} shadow;

void main() {
    mat4 matrix = shadow.index == 0 ? scene.sun_shadow_matrix
                                    : spot_buffer.lights[shadow.index - 1].shadow_matrix;
    gl_Position = matrix * object_data.model * vec4(position, 1.0);
}
