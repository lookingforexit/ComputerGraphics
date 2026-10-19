#version 450
#extension GL_GOOGLE_include_directive : require
#include "uniforms.glsl"

layout(location = 0) in vec3 in_position;

void main() {
    gl_Position = scene.view_projection * object_data.model * vec4(in_position, 1.0);
}
