#pragma once

#include "graphics_internal.hpp"

struct GLFWwindow;

namespace application {

bool initialize();
void shutdown();

void update(double time, GLFWwindow* window);
void render(const graphics::internal::FrameData& fd);

} // namespace application
