#pragma once

#include "render_data.hpp"

namespace geometry {

graphics::core::MeshData make_sphere(uint32_t longitude_segments, uint32_t latitude_segments);
graphics::core::MeshData make_plane();
void append_spotlight_cone(std::vector<graphics::core::Vertex> &lines,
                           const graphics::core::SpotLight &light);

}
