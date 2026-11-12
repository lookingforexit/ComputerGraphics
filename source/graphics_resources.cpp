#include "graphics_resources.hpp"

#include <cstdio>
#include <cstdlib>
#include <csetjmp>
#include <cstring>
#include <iostream>
#include <vector>
#include <jpeglib.h>
#include <png.h>

namespace graphics::core::detail {
bool create_mapped_buffer(VkDeviceSize size, VkBufferUsageFlags usage, GPUBuffer &result) {
    const auto &context = internal::context;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocation_info{};
    allocation_info.flags =
        VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    allocation_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VmaAllocationInfo allocation_result{};

    const VkResult status = vmaCreateBuffer(context.allocator, &buffer_info, &allocation_info,
                                            &result.buffer, &result.allocation, &allocation_result);

    if (status != VK_SUCCESS) {
        std::cerr << "cant create vulkan buffer\n";
        return false;
    }

    result.mapped = allocation_result.pMappedData;
    return true;
}

void destroy_buffer(GPUBuffer &buffer) {
    if (buffer.buffer == VK_NULL_HANDLE) {
        return;
    }

    const auto &context = internal::context;

    vmaDestroyBuffer(context.allocator, buffer.buffer, buffer.allocation);
    buffer = {};
}

bool write_buffer(GPUBuffer &buffer, const void *data, VkDeviceSize size, VkDeviceSize offset) {
    if (buffer.mapped == nullptr) {
        return false;
    }

    std::memcpy(static_cast<char *>(buffer.mapped) + offset, data, size);

    return vmaFlushAllocation(internal::context.allocator, buffer.allocation, offset, size) ==
           VK_SUCCESS;
}

bool create_device_buffer(VkDeviceSize size, VkBufferUsageFlags usage, GPUBuffer &result) {
    const auto &context = internal::context;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocation_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    return vmaCreateBuffer(context.allocator, &buffer_info, &allocation_info,
                            &result.buffer, &result.allocation, nullptr) == VK_SUCCESS;
}

bool submit_transfer(const std::function<void(VkCommandBuffer)> &record) {
    const auto &context = internal::context;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = context.graphics_queue_index;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(context.device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    bool success = vkAllocateCommandBuffers(context.device, &allocate, &command) == VK_SUCCESS;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (success) {
        success = vkBeginCommandBuffer(command, &begin) == VK_SUCCESS;
    }
    if (success) {
        record(command);
        success = vkEndCommandBuffer(command) == VK_SUCCESS;
    }
    if (success) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        success = vkQueueSubmit(context.graphics_queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        if (success) {
            success = vkQueueWaitIdle(context.graphics_queue) == VK_SUCCESS;
        }
    }
    vkDestroyCommandPool(context.device, pool, nullptr);
    return success;
}

bool upload_device_buffer(const void *data, VkDeviceSize size, VkBufferUsageFlags usage,
                           GPUBuffer &result) {
    GPUBuffer staging;
    if (!create_mapped_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) {
        return false;
    }
    const bool success = write_buffer(staging, data, size) &&
        create_device_buffer(size, usage, result) && submit_transfer([&](VkCommandBuffer command) {
            const VkBufferCopy copy{0, 0, size};
            vkCmdCopyBuffer(command, staging.buffer, result.buffer, 1, &copy);
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = result.buffer;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
                                  0, nullptr, 1, &barrier, 0, nullptr);
        });
    destroy_buffer(staging);
    if (!success) {
        destroy_buffer(result);
    }
    return success;
}

namespace {
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<unsigned char> pixels;
};

struct JPEGError {
    jpeg_error_mgr manager;
    std::jmp_buf jump;
};

void on_jpeg_error(j_common_ptr decoder) {
    auto *error = reinterpret_cast<JPEGError *>(decoder->err);
    std::longjmp(error->jump, 1);
}

bool read_jpeg(const char *path, Image &image) {
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    jpeg_decompress_struct decoder{};
    JPEGError error{};
    decoder.err = jpeg_std_error(&error.manager);
    error.manager.error_exit = on_jpeg_error;
    // Keep C allocations in the decoder: libjpeg reports errors through longjmp.
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        return false;
    }
    jpeg_create_decompress(&decoder);
    jpeg_stdio_src(&decoder, file);
    jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    image.width = decoder.output_width;
    image.height = decoder.output_height;
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4);
    JSAMPARRAY row = (*decoder.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&decoder),
        JPOOL_IMAGE, image.width * 3, 1);
    while (decoder.output_scanline < decoder.output_height) {
        const size_t y = decoder.output_scanline;
        jpeg_read_scanlines(&decoder, row, 1);
        for (uint32_t x = 0; x < image.width; ++x) {
            const size_t offset = (y * image.width + x) * 4;
            std::memcpy(image.pixels.data() + offset, row[0] + x * 3, 3);
            image.pixels[offset + 3] = 255;
        }
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    std::fclose(file);
    return true;
}

bool read_image(const char *path, Image &image) {
    const char *extension = std::strrchr(path, '.');
    if (extension != nullptr && (std::strcmp(extension, ".jpeg") == 0 ||
                                std::strcmp(extension, ".jpg") == 0)) {
        return read_jpeg(path, image);
    }
    png_image decoder{};
    decoder.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&decoder, path)) {
        png_image_free(&decoder);
        return false;
    }
    decoder.format = PNG_FORMAT_RGBA;
    image.width = decoder.width;
    image.height = decoder.height;
    image.pixels.resize(PNG_IMAGE_SIZE(decoder));
    const bool success = png_image_finish_read(&decoder, nullptr, image.pixels.data(), 0, nullptr);
    png_image_free(&decoder);
    return success;
}
}

bool load_texture(const char *path, VkFormat format, Texture &result) {
    const auto &context = internal::context;
    Image image;
    if (!read_image(path, image)) {
        std::cerr << "unable to decode texture: " << path << '\n';
        return false;
    }
    GPUBuffer staging;
    const VkDeviceSize bytes = image.pixels.size();
    if (!create_mapped_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) {
        return false;
    }
    if (!write_buffer(staging, image.pixels.data(), bytes)) {
        destroy_buffer(staging);
        return false;
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {image.width, image.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocation_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    bool success = vmaCreateImage(context.allocator, &image_info, &allocation_info,
                                  &result.image, &result.allocation, nullptr) == VK_SUCCESS;
    if (success) {
        success = submit_transfer([&](VkCommandBuffer command) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = result.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                  0, nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = image_info.extent;
            vkCmdCopyBufferToImage(command, staging.buffer, result.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                  0, nullptr, 0, nullptr, 1, &barrier);
        });
    }
    destroy_buffer(staging);
    if (success) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = result.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        success = vkCreateImageView(context.device, &view, nullptr, &result.view) == VK_SUCCESS;
    }
    if (!success) {
        destroy_texture(result);
    }
    return success;
}

void destroy_texture(Texture &texture) {
    const auto &context = internal::context;
    if (texture.view != VK_NULL_HANDLE) {
        vkDestroyImageView(context.device, texture.view, nullptr);
    }
    if (texture.image != VK_NULL_HANDLE) {
        vmaDestroyImage(context.allocator, texture.image, texture.allocation);
    }
    texture = {};
}

}
