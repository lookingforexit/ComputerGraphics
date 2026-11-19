#include "graphics.hpp"
#include "graphics_pipeline.hpp"
#include "graphics_gpu_data.hpp"
#include "graphics_resources.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <glm/gtc/matrix_transform.hpp>

namespace graphics::core {
namespace {
using detail::GPUBuffer;
using detail::create_mapped_buffer;
using detail::write_buffer;
using detail::destroy_buffer;

struct Mesh {
    GPUBuffer vertices;
    GPUBuffer indices;
    uint32_t index_count = 0;
};

Mesh sphere_mesh;
Mesh plane_mesh;

Mesh &get_mesh(MeshKind kind) {
    switch (kind) {
        case MeshKind::Sphere: return sphere_mesh;
        case MeshKind::Plane: return plane_mesh;
    }
    return sphere_mesh;
}
GPUBuffer visualization_line_buffer;
GPUBuffer visualization_line_staging;
GPUBuffer scene_uniform_buffer;
GPUBuffer point_light_buffer;
GPUBuffer spot_light_buffer;
VkDeviceSize point_light_capacity = 1;
VkDeviceSize spot_light_capacity = 1;
GPUBuffer model_uniform_buffer;
VkDeviceSize model_stride = 0;
size_t model_capacity = 0;

constexpr size_t material_count = static_cast<size_t>(TextureKind::Count);
enum TextureImage : uint32_t {
    hamster_image, black_image, hamster_specular_image, hamster_emissive_image,
    tiles_image, waves_image, waves_specular_image, texture_image_count,
};
std::array<detail::Texture, texture_image_count> textures;
std::array<VkDescriptorSet, material_count> material_sets{};
VkSampler texture_sampler = VK_NULL_HANDLE;
const TextureImage material_images[material_count][3] = {
    {hamster_image, black_image, black_image},
    {hamster_image, hamster_specular_image, hamster_emissive_image},
    {tiles_image, black_image, black_image},
    {waves_image, waves_specular_image, black_image},
};

VkDescriptorSetLayout scene_set_layout;
VkDescriptorSetLayout model_set_layout;
VkDescriptorPool descriptor_pool;

VkDescriptorSet scene_set;

VkPipelineLayout pipeline_layout;
detail::Pipelines pipelines;
VkDeviceSize visualization_line_capacity = 0;

constexpr uint32_t shadow_size = 2048;
std::array<detail::Texture, 2> shadow_maps;
VkSampler shadow_sampler = VK_NULL_HANDLE;
std::vector<VkImageView> spot_shadow_views;

bool create_shadow_image(detail::Texture &map, uint32_t layers, bool array) {
    const auto &context = internal::context;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_D32_SFLOAT;
    info.extent = {shadow_size, shadow_size, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(context.allocator, &info, &allocation, &map.image, &map.allocation, nullptr) != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = map.image;
    view.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(context.device, &view, nullptr, &map.view) != VK_SUCCESS) {
        return false;
    }
    return true;
}

void destroy_spot_shadow_views() {
    for (VkImageView view : spot_shadow_views) {
        vkDestroyImageView(internal::context.device, view, nullptr);
    }
    spot_shadow_views.clear();
}

bool resize_spot_shadows(size_t count) {
    count = std::max<size_t>(count, 1);
    if (count == spot_shadow_views.size()) {
        return true;
    }
    const auto &context = internal::context;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context.physical_device, &properties);
    if (count > properties.limits.maxImageArrayLayers) {
        std::cerr << "too many spot shadow layers\n";
        return false;
    }
    detail::Texture replacement;
    std::vector<VkImageView> views(count, VK_NULL_HANDLE);
    bool success = create_shadow_image(replacement, static_cast<uint32_t>(count), true);
    for (uint32_t i = 0; success && i < count; ++i) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = replacement.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_D32_SFLOAT;
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
        success = vkCreateImageView(context.device, &view, nullptr, &views[i]) == VK_SUCCESS;
    }
    if (!success) {
        for (VkImageView view : views) {
            vkDestroyImageView(context.device, view, nullptr);
        }
        detail::destroy_texture(replacement);
        return false;
    }
    vkQueueWaitIdle(context.graphics_queue);
    destroy_spot_shadow_views();
    detail::destroy_texture(shadow_maps[1]);
    shadow_maps[1] = replacement;
    spot_shadow_views = std::move(views);
    if (scene_set != VK_NULL_HANDLE) {
        VkDescriptorImageInfo image{shadow_sampler, shadow_maps[1].view,
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = scene_set;
        write.dstBinding = 4;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
    }
    return true;
}

bool create_shadow_maps() {
    const auto &context = internal::context;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(context.physical_device, VK_FORMAT_D32_SFLOAT, &properties);
    const auto required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((properties.optimalTilingFeatures & required) != required) {
        return false;
    }
    if (!create_shadow_image(shadow_maps[0], 1, false) || !resize_spot_shadows(1)) {
        return false;
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampler.compareEnable = VK_TRUE;
    sampler.compareOp = VK_COMPARE_OP_LESS;
    return vkCreateSampler(context.device, &sampler, nullptr, &shadow_sampler) == VK_SUCCESS;
}

void record_shadows(VkCommandBuffer command, const RenderFrame &frame) {
    for (uint32_t index = 0; index <= spot_shadow_views.size(); ++index) {
        const uint32_t layer = index == 0 ? 0 : index - 1;
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = shadow_maps[index == 0 ? 0 : 1].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = index == 0 ? shadow_maps[0].view : spot_shadow_views[layer];
        depth.imageLayout = barrier.newLayout;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = {shadow_size, shadow_size};
        rendering.layerCount = 1;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(command, &rendering);
        VkViewport viewport{0, 0, float(shadow_size), float(shadow_size), 0, 1};
        VkRect2D scissor{{0, 0}, {shadow_size, shadow_size}};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdSetDepthBias(command, frame.shadows.depth_bias, 0, frame.shadows.slope_bias);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.shadow);
        vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 16, sizeof(index), &index);
        for (size_t i = 0; i < frame.objects.size(); ++i) {
            const auto &object = frame.objects[i];
            if (object.shading != Shading::Lit || (index == 1 && frame.spot_lights.empty())) {
                continue;
            }
            const auto &mesh = get_mesh(object.mesh);
            const VkDeviceSize offset = 0;
            const uint32_t dynamic_offset = static_cast<uint32_t>(i * model_stride);
            const VkDescriptorSet sets[] = {scene_set, material_sets[static_cast<size_t>(object.material.texture)]};
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                     0, 2, sets, 1, &dynamic_offset);
            vkCmdBindVertexBuffers(command, 0, 1, &mesh.vertices.buffer, &offset);
            vkCmdBindIndexBuffer(command, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command, mesh.index_count, 1, 0, 0, 0);
        }
        vkCmdEndRendering(command);
        barrier.oldLayout = barrier.newLayout;
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
}

bool ensure_visualization_line_capacity(size_t vertex_count) {
    const VkDeviceSize requested = vertex_count * sizeof(Vertex);
    if (requested <= visualization_line_capacity) {
        return true;
    }
    if (internal::context.graphics_queue != VK_NULL_HANDLE) {
        vkQueueWaitIdle(internal::context.graphics_queue);
    }
    GPUBuffer replacement{};
    const VkDeviceSize new_capacity =
        std::max<VkDeviceSize>(requested, visualization_line_capacity * 2);
    GPUBuffer staging;
    if (!detail::create_device_buffer(new_capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, replacement) ||
        !create_mapped_buffer(new_capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) {
        destroy_buffer(replacement);
        destroy_buffer(staging);
        return false;
    }
    destroy_buffer(visualization_line_buffer);
    visualization_line_buffer = replacement;
    destroy_buffer(visualization_line_staging);
    visualization_line_staging = staging;
    visualization_line_capacity = new_capacity;
    return true;
}

template<typename Light>
bool ensure_light_buffer(GPUBuffer &buffer, VkDeviceSize &capacity, VkDeviceSize requested,
                         uint32_t binding) {
    requested = std::max<VkDeviceSize>(requested, 1);
    if (requested <= capacity) {
        return true;
    }

    if (internal::context.graphics_queue != VK_NULL_HANDLE) {
        vkQueueWaitIdle(internal::context.graphics_queue);
    }
    const VkDeviceSize new_capacity = std::max(requested, capacity * 2);
    GPUBuffer replacement{};
    if (!create_mapped_buffer(new_capacity * sizeof(Light),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, replacement)) {
        return false;
    }
    destroy_buffer(buffer);
    buffer = replacement;
    capacity = new_capacity;

    VkDescriptorBufferInfo info{};
    info.buffer = buffer.buffer;
    info.range = capacity * sizeof(Light);
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = scene_set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(internal::context.device, 1, &write, 0, nullptr);
    return true;
}

void update_material_descriptors() {
    const auto &context = internal::context;
    const VkDescriptorBufferInfo model_info{model_uniform_buffer.buffer, 0, sizeof(gpu::ObjectUniforms)};
    for (size_t material = 0; material < material_count; ++material) {
        std::array<VkDescriptorImageInfo, 3> images{};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (uint32_t binding = 0; binding < writes.size(); ++binding) {
            auto &write = writes[binding];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = material_sets[material];
            write.dstBinding = binding;
            write.descriptorCount = 1;
            if (binding == 0) {
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                write.pBufferInfo = &model_info;
            } else {
                images[binding - 1] = {texture_sampler,
                    textures[material_images[material][binding - 1]].view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &images[binding - 1];
            }
        }
        vkUpdateDescriptorSets(context.device, static_cast<uint32_t>(writes.size()),
                                writes.data(), 0, nullptr);
    }
}

bool ensure_model_capacity(size_t count) {
    if (count <= model_capacity) {
        return true;
    }
    const size_t new_capacity = std::max(count, std::max<size_t>(model_capacity * 2, 16));
    GPUBuffer replacement;
    if (!create_mapped_buffer(model_stride * new_capacity, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              replacement)) {
        return false;
    }
    vkQueueWaitIdle(internal::context.graphics_queue);
    destroy_buffer(model_uniform_buffer);
    model_uniform_buffer = replacement;
    model_capacity = new_capacity;
    update_material_descriptors();
    return true;
}

bool create_textures() {
    const char *paths[] = {"homyak.jpeg", "textures/black.png", "textures/hamster_specular.png",
        "textures/hamster_emissive.png", "textures/tiles.png", "textures/waves.png",
        "textures/waves_specular.png"};
    for (size_t i = 0; i < textures.size(); ++i) {
        const bool linear = i == black_image || i == hamster_specular_image || i == waves_specular_image;
        if (!detail::load_texture(paths[i], linear ? VK_FORMAT_R8G8B8A8_UNORM :
                                                    VK_FORMAT_R8G8B8A8_SRGB, textures[i])) {
            return false;
        }
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    return vkCreateSampler(internal::context.device, &sampler, nullptr, &texture_sampler) == VK_SUCCESS;
}

bool create_descriptor_resources() {
    const auto &context = internal::context;

    const VkDescriptorSetLayoutBinding scene_bindings[] = {
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {gpu::scene_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {gpu::point_lights_binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {gpu::spot_lights_binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding model_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 5;
    layout_info.pBindings = scene_bindings;

    if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr, &scene_set_layout) !=
        VK_SUCCESS) {
        std::cerr << "unable to create scene set layout\n";
        return false;
    }

    layout_info.bindingCount = 4;
    layout_info.pBindings = model_bindings;
    if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr, &model_set_layout) !=
        VK_SUCCESS) {
        std::cerr << "unable to create model set layout\n";
        return false;
    }

    const VkDescriptorSetLayout layouts_for_pipeline[] = {scene_set_layout, model_set_layout};

    const VkPushConstantRange sampling_ranges[] = {
        {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gpu::SamplingConstants)},
        {VK_SHADER_STAGE_VERTEX_BIT, 16, sizeof(int)},
    };
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.pushConstantRangeCount = 2;
    pipeline_layout_info.pPushConstantRanges = sampling_ranges;
    pipeline_layout_info.setLayoutCount = 2;
    pipeline_layout_info.pSetLayouts = layouts_for_pipeline;

    if (vkCreatePipelineLayout(context.device, &pipeline_layout_info, nullptr, &pipeline_layout) !=
        VK_SUCCESS) {
        std::cerr << "unable to create pipeline layout\n";
        return false;
    }

    constexpr uint32_t descriptor_set_capacity = 1 + material_count;
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, material_count},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, material_count * 3 + 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
    };

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = descriptor_set_capacity;
    pool_info.poolSizeCount = 4;
    pool_info.pPoolSizes = pool_sizes;

    if (vkCreateDescriptorPool(context.device, &pool_info, nullptr, &descriptor_pool) !=
        VK_SUCCESS) {
        std::cerr << "unable to create descriptor pool\n";
        return false;
    }

    if (!create_mapped_buffer(sizeof(gpu::SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              scene_uniform_buffer) ||
        !create_mapped_buffer(sizeof(gpu::PointLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              point_light_buffer) ||
        !create_mapped_buffer(sizeof(gpu::SpotLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              spot_light_buffer)) {
        return false;
    }

    VkDescriptorSetAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = descriptor_pool;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &scene_set_layout;
    if (vkAllocateDescriptorSets(context.device, &allocate_info, &scene_set) != VK_SUCCESS) {
        std::cerr << "unable to allocate scene descriptor set\n";
        return false;
    }

    const VkDescriptorBufferInfo buffer_infos[] = {
        {scene_uniform_buffer.buffer, 0, sizeof(gpu::SceneUniforms)},
        {point_light_buffer.buffer, 0, sizeof(gpu::PointLight)},
        {spot_light_buffer.buffer, 0, sizeof(gpu::SpotLight)},
    };
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t binding = 0; binding < 3; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = scene_set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType =
            binding == gpu::scene_binding ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &buffer_infos[binding];
    }

    vkUpdateDescriptorSets(context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);

    for (uint32_t i = 0; i < shadow_maps.size(); ++i) {
        VkDescriptorImageInfo image{shadow_sampler, shadow_maps[i].view,
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = scene_set;
        write.dstBinding = 3 + i;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
    }

    std::array<VkDescriptorSetLayout, material_count> material_layouts;
    material_layouts.fill(model_set_layout);
    allocate_info.descriptorSetCount = material_count;
    allocate_info.pSetLayouts = material_layouts.data();
    if (vkAllocateDescriptorSets(context.device, &allocate_info, material_sets.data()) != VK_SUCCESS) {
        return false;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context.physical_device, &properties);
    const VkDeviceSize alignment = properties.limits.minUniformBufferOffsetAlignment;
    model_stride = (sizeof(gpu::ObjectUniforms) + alignment - 1) / alignment * alignment;
    return ensure_model_capacity(16);
}

glm::vec3 normalized_light_direction(const glm::vec3 &direction) {
    constexpr float minimum_length_squared = 1e-8f;
    const float length_squared = glm::dot(direction, direction);
    if (length_squared < minimum_length_squared) {
        return {0.0f, -1.0f, 0.0f};
    }
    return direction / std::sqrt(length_squared);
}

gpu::SceneUniforms make_scene_uniforms(const RenderFrame &frame) {
    return {
        .view_projection = frame.view_projection,
        .camera_position = frame.camera_position,
        .ambient_color = frame.ambient.color,
        .ambient_intensity = frame.ambient.intensity,
        .directional_direction = normalized_light_direction(frame.directional.direction),
        .directional_color = frame.directional.color,
        .directional_intensity = frame.directional.intensity,
        .point_light_count = static_cast<uint32_t>(frame.point_lights.size()),
        .spot_light_count = static_cast<uint32_t>(frame.spot_lights.size()),
    };
}

gpu::ObjectUniforms make_object_uniforms(const RenderObject &object) {
    return {
        .model = object.model,
        .albedo = object.material.albedo,
        .specular_color = object.material.specular_color,
        .shininess = object.material.shininess,
        .use_vertex_color = object.material.use_vertex_color ? 1u : 0u,
    };
}

std::vector<gpu::PointLight> make_point_light_data(const std::vector<PointLight> &lights) {
    std::vector<gpu::PointLight> data;
    data.reserve(std::max<size_t>(lights.size(), 1));
    for (const PointLight &light : lights) {
        data.push_back({
            .position = light.position,
            .color = light.color,
            .intensity = light.intensity,
        });
    }
    if (data.empty()) {
        data.emplace_back();
    }
    return data;
}

std::vector<gpu::SpotLight> make_spot_light_data(const std::vector<SpotLight> &lights) {
    std::vector<gpu::SpotLight> data;
    data.reserve(std::max<size_t>(lights.size(), 1));
    for (const SpotLight &light : lights) {
        data.push_back({
            .position = light.position,
            .direction = normalized_light_direction(light.direction),
            .color = light.color,
            .intensity = light.intensity,
            .inner_cone_cosine = std::cos(glm::radians(light.inner_angle_degrees)),
            .outer_cone_cosine = std::cos(glm::radians(light.outer_angle_degrees)),
        });
    }
    if (data.empty()) {
        data.emplace_back();
    }
    return data;
}

bool upload_frame_data(const RenderFrame &frame) {
    const size_t model_count = frame.objects.size() + (frame.light_lines.empty() ? 0 : 1);
    if (!resize_spot_shadows(frame.spot_lights.size()) || !ensure_model_capacity(model_count) ||
        (!frame.light_lines.empty() && !ensure_visualization_line_capacity(frame.light_lines.size())) ||
        !ensure_light_buffer<gpu::PointLight>(point_light_buffer, point_light_capacity,
                                              frame.point_lights.size(), gpu::point_lights_binding) ||
        !ensure_light_buffer<gpu::SpotLight>(spot_light_buffer, spot_light_capacity,
                                             frame.spot_lights.size(), gpu::spot_lights_binding)) {
        std::cerr << "unable to grow GPU data buffers\n";
        return false;
    }

    auto scene = make_scene_uniforms(frame);
    const auto light_view = [](glm::vec3 position, glm::vec3 direction) {
        direction = normalized_light_direction(direction);
        const glm::vec3 up = std::abs(direction.y) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        return glm::lookAt(position, position + direction, up);
    };
    const float half_extent = frame.shadows.extent * 0.5f;
    auto projection = glm::ortho(-half_extent, half_extent, -half_extent, half_extent, 0.1f, 60.0f);
    projection[1][1] *= -1.0f;
    scene.sun_shadow_matrix = projection * light_view(-scene.directional_direction * 25.0f,
                                                       scene.directional_direction);
    const auto points = make_point_light_data(frame.point_lights);
    auto spots = make_spot_light_data(frame.spot_lights);
    for (size_t i = 0; i < frame.spot_lights.size(); ++i) {
        const auto &light = frame.spot_lights[i];
        projection = glm::perspective(glm::radians(std::clamp(light.outer_angle_degrees * 2.0f, 1.0f, 178.0f)),
                                      1.0f, 0.1f, 60.0f);
        projection[1][1] *= -1.0f;
        spots[i].shadow_matrix = projection * light_view(light.position, light.direction);
    }
    if (!write_buffer(scene_uniform_buffer, &scene, sizeof(scene)) ||
        !write_buffer(point_light_buffer, points.data(), points.size() * sizeof(gpu::PointLight)) ||
        !write_buffer(spot_light_buffer, spots.data(), spots.size() * sizeof(gpu::SpotLight))) {
        return false;
    }

    for (size_t i = 0; i < frame.objects.size(); ++i) {
        const auto object = make_object_uniforms(frame.objects[i]);
        if (!write_buffer(model_uniform_buffer, &object, sizeof(object), i * model_stride)) {
            return false;
        }
    }
    if (!frame.light_lines.empty()) {
        const size_t line_model_index = frame.objects.size();
        const gpu::ObjectUniforms line_object{};
        if (!write_buffer(model_uniform_buffer, &line_object, sizeof(line_object),
                          line_model_index * model_stride) ||
            !write_buffer(visualization_line_staging, frame.light_lines.data(),
                          frame.light_lines.size() * sizeof(Vertex))) {
            return false;
        }
    }
    return true;
}

void record_draw_commands(const internal::FrameData &fd, const RenderFrame &frame) {
    const auto &context = internal::context;

    vkResetCommandBuffer(fd.command_buffer, 0);

    VkCommandBufferBeginInfo command_begin{};
    command_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(fd.command_buffer, &command_begin) != VK_SUCCESS) {
        std::cerr << "unable to start writing command buffer\n";
        return;
    }

    if (!frame.light_lines.empty()) {
        const VkBufferCopy copy{0, 0, frame.light_lines.size() * sizeof(Vertex)};
        vkCmdCopyBuffer(fd.command_buffer, visualization_line_staging.buffer,
                         visualization_line_buffer.buffer, 1, &copy);
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(fd.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
                              1, &barrier, 0, nullptr, 0, nullptr);
    }

    record_shadows(fd.command_buffer, frame);

    VkClearValue clear_values[2]{};
    clear_values[0].color = {{0.04f, 0.05f, 0.08f, 1.0f}};
    clear_values[1].depthStencil = {.depth = 1.0f, .stencil = 0};

    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = context.render_pass;
    render_begin.framebuffer = fd.framebuffer;
    render_begin.renderArea.extent = context.swapchain_extent;
    render_begin.clearValueCount = 2;
    render_begin.pClearValues = clear_values;

    vkCmdBeginRenderPass(fd.command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = context.swapchain_extent.width;
    viewport.height = context.swapchain_extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.extent = context.swapchain_extent;

    vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);

    constexpr VkDeviceSize vertex_offset = 0;

    for (size_t i = 0; i < frame.objects.size(); ++i) {
        const RenderObject &object = frame.objects[i];
        const Mesh &mesh = get_mesh(object.mesh);
        if (mesh.vertices.buffer == VK_NULL_HANDLE || mesh.indices.buffer == VK_NULL_HANDLE) {
            continue;
        }
        const VkPipeline pipeline = object.shading == Shading::Lit ? pipelines.lit : pipelines.unlit;
        vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &mesh.vertices.buffer, &vertex_offset);
        vkCmdBindIndexBuffer(fd.command_buffer, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        const auto material = static_cast<size_t>(object.material.texture);
        const VkDescriptorSet sets[] = {scene_set, material_sets[material]};
        const uint32_t dynamic_offset = static_cast<uint32_t>(i * model_stride);
        const gpu::SamplingConstants sampling{
            object.sampling.animated ? 1u : 0u, frame.time, object.sampling.strength};
        vkCmdPushConstants(fd.command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(sampling), &sampling);

        vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                0, 2, sets, 1, &dynamic_offset);
        vkCmdDrawIndexed(fd.command_buffer, mesh.index_count, 1, 0, 0, 0);
    }

    if (!frame.light_lines.empty()) {
        const size_t line_model_index = frame.objects.size();
        vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.lines);
        vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &visualization_line_buffer.buffer,
                               &vertex_offset);
        const VkDescriptorSet sets[] = {scene_set, material_sets[0]};
        const uint32_t dynamic_offset = static_cast<uint32_t>(line_model_index * model_stride);
        vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                0, 2, sets, 1, &dynamic_offset);
        vkCmdDraw(fd.command_buffer, static_cast<uint32_t>(frame.light_lines.size()), 1, 0, 0);
    }

    vkCmdEndRenderPass(fd.command_buffer);

    if (vkEndCommandBuffer(fd.command_buffer) != VK_SUCCESS) {
        std::cerr << "unable to finish writing command buffer\n";
    }
}
}

bool initialize() {
    if (!create_shadow_maps() || !create_textures() || !create_descriptor_resources() ||
        !detail::create_pipelines(internal::context.device, internal::context.render_pass,
                                  pipeline_layout, pipelines)) {
        shutdown();
        return false;
    }

    return true;
}

void shutdown() {
    const auto &context = internal::context;

    if (context.device == VK_NULL_HANDLE) {
        return;
    }

    if (context.graphics_queue != VK_NULL_HANDLE) {
        vkQueueWaitIdle(context.graphics_queue);
    }

    destroy_spot_shadow_views();
    for (auto &map : shadow_maps) {
        detail::destroy_texture(map);
    }
    vkDestroySampler(context.device, shadow_sampler, nullptr);
    shadow_sampler = VK_NULL_HANDLE;
    detail::destroy_pipelines(context.device, pipelines);

    if (pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
    }

    if (descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context.device, descriptor_pool, nullptr);
        descriptor_pool = VK_NULL_HANDLE;
    }

    destroy_buffer(scene_uniform_buffer);
    destroy_buffer(point_light_buffer);
    destroy_buffer(spot_light_buffer);
    destroy_buffer(model_uniform_buffer);
    model_capacity = 0;
    model_stride = 0;
    material_sets.fill(VK_NULL_HANDLE);
    for (auto &texture : textures) {
        detail::destroy_texture(texture);
    }
    if (texture_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(context.device, texture_sampler, nullptr);
        texture_sampler = VK_NULL_HANDLE;
    }
    destroy_buffer(sphere_mesh.vertices);
    destroy_buffer(sphere_mesh.indices);
    destroy_buffer(plane_mesh.vertices);
    destroy_buffer(plane_mesh.indices);
    sphere_mesh = {};
    plane_mesh = {};
    destroy_buffer(visualization_line_buffer);
    destroy_buffer(visualization_line_staging);

    if (scene_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context.device, scene_set_layout, nullptr);
        scene_set_layout = VK_NULL_HANDLE;
    }

    if (model_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context.device, model_set_layout, nullptr);
        model_set_layout = VK_NULL_HANDLE;
    }

    scene_set = VK_NULL_HANDLE;
    point_light_capacity = 1;
    spot_light_capacity = 1;
    visualization_line_capacity = 0;
}

bool upload_mesh(MeshKind kind, const MeshData &data) {
    if (data.vertices.empty() || data.indices.empty()) {
        std::cerr << "mesh vertices or indices are empty\n";
        return false;
    }

    Mesh replacement;
    const VkDeviceSize vertex_bytes = data.vertices.size() * sizeof(Vertex);
    const VkDeviceSize index_bytes = data.indices.size() * sizeof(uint32_t);
    if (!detail::upload_device_buffer(data.vertices.data(), vertex_bytes,
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, replacement.vertices) ||
        !detail::upload_device_buffer(data.indices.data(), index_bytes,
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT, replacement.indices)) {
        destroy_buffer(replacement.vertices);
        destroy_buffer(replacement.indices);
        return false;
    }
    replacement.index_count = static_cast<uint32_t>(data.indices.size());

    Mesh &mesh = get_mesh(kind);
    if (mesh.vertices.buffer != VK_NULL_HANDLE || mesh.indices.buffer != VK_NULL_HANDLE) {
        vkQueueWaitIdle(internal::context.graphics_queue);
    }
    destroy_buffer(mesh.vertices);
    destroy_buffer(mesh.indices);
    mesh = replacement;
    return true;
}

void render(const internal::FrameData &fd, const RenderFrame &frame) {
    if (fd.command_buffer == VK_NULL_HANDLE || fd.framebuffer == VK_NULL_HANDLE ||
        pipelines.lit == VK_NULL_HANDLE) {
        return;
    }
    if (!upload_frame_data(frame)) {
        return;
    }
    record_draw_commands(fd, frame);
}

}
