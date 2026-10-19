#pragma once

#include <vulkan/vulkan.h>

namespace graphics::core::detail {

struct Pipelines {
    VkPipeline lit = VK_NULL_HANDLE;
    VkPipeline unlit = VK_NULL_HANDLE;
    VkPipeline lines = VK_NULL_HANDLE;
};

bool create_pipelines(VkDevice device, VkRenderPass render_pass, VkPipelineLayout layout,
                      Pipelines &pipelines);
void destroy_pipelines(VkDevice device, Pipelines &pipelines);

}
