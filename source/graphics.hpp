#pragma once

#include <vector>
#include "graphics_internal.hpp"

namespace graphics::core {
    inline constexpr uint32_t kMaxObjectCount = 3;

    struct SceneUniforms {
        float projection[4][4];
        float view[4][4];
    };

    struct ModelUniforms {
        float model[4][4];
        float color[4];
    };

    struct Vertex {
        float position[3];
        float color[3];
    };

    bool initialize();
    void shutdown();

    bool getGeometry(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

    void render(const internal::FrameData& fd, const SceneUniforms& scene, const std::vector<ModelUniforms>& objects);
}
