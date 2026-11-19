#version 450
#extension GL_GOOGLE_include_directive : require
#include "uniforms.glsl"

layout(location = 0) in vec3 position;
layout(push_constant) uniform ShadowConstants {
    layout(offset = 16) int index;
} shadow;

void main() {
    gl_Position = scene.shadow_matrices[shadow.index] * object_data.model * vec4(position, 1.0);
}
