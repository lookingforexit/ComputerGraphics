#pragma once

#include "graphics_internal.hpp"

#include <functional>

namespace graphics::core::detail {

struct GPUBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void *mapped = nullptr;
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

bool create_mapped_buffer(VkDeviceSize size, VkBufferUsageFlags usage, GPUBuffer &result);
bool create_device_buffer(VkDeviceSize size, VkBufferUsageFlags usage, GPUBuffer &result);
bool write_buffer(GPUBuffer &buffer, const void *data, VkDeviceSize size, VkDeviceSize offset = 0);
void destroy_buffer(GPUBuffer &buffer);

bool submit_transfer(const std::function<void(VkCommandBuffer)> &record);
bool upload_device_buffer(const void *data, VkDeviceSize size, VkBufferUsageFlags usage,
                           GPUBuffer &result);
bool load_texture(const char *path, VkFormat format, Texture &result);
void destroy_texture(Texture &texture);

}
