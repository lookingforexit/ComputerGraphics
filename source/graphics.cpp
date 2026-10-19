#include "graphics.hpp"
#include "graphics_pipeline.hpp"
#include "graphics_gpu_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

namespace graphics::core {
namespace {
struct GPUBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void *mapped = nullptr;
};

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
GPUBuffer scene_uniform_buffer;
GPUBuffer point_light_buffer;
GPUBuffer spot_light_buffer;
VkDeviceSize point_light_capacity = 1;
VkDeviceSize spot_light_capacity = 1;
std::vector<GPUBuffer> model_uniforms_buffers;

VkDescriptorSetLayout scene_set_layout;
VkDescriptorSetLayout model_set_layout;
VkDescriptorPool descriptor_pool;

VkDescriptorSet scene_set;
std::vector<VkDescriptorSet> model_sets;

VkPipelineLayout pipeline_layout;
detail::Pipelines pipelines;
VkDeviceSize visualization_line_capacity = 0;

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

bool write_buffer(GPUBuffer &buffer, const void *data, VkDeviceSize size) {
    if (buffer.mapped == nullptr) {
        return false;
    }

    std::memcpy(buffer.mapped, data, size);

    return vmaFlushAllocation(internal::context.allocator, buffer.allocation, 0, size) ==
           VK_SUCCESS;
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
    if (!create_mapped_buffer(new_capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, replacement)) {
        return false;
    }
    destroy_buffer(visualization_line_buffer);
    visualization_line_buffer = replacement;
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

bool ensure_model_capacity(size_t count) {
    const auto &context = internal::context;
    while (model_uniforms_buffers.size() < count) {
        GPUBuffer buffer{};
        if (!create_mapped_buffer(sizeof(gpu::ObjectUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  buffer)) {
            return false;
        }

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = descriptor_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &model_set_layout;
        if (vkAllocateDescriptorSets(context.device, &allocate, &set) != VK_SUCCESS) {
            destroy_buffer(buffer);
            std::cerr << "unable to allocate a model descriptor set\n";
            return false;
        }

        VkDescriptorBufferInfo info{buffer.buffer, 0, sizeof(gpu::ObjectUniforms)};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
        model_uniforms_buffers.push_back(buffer);
        model_sets.push_back(set);
    }
    return true;
}

bool create_descriptor_resources() {
    const auto &context = internal::context;

    const VkDescriptorSetLayoutBinding scene_bindings[] = {
        {gpu::scene_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {gpu::point_lights_binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {gpu::spot_lights_binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding model_binding = {
        0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = scene_bindings;

    if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr, &scene_set_layout) !=
        VK_SUCCESS) {
        std::cerr << "unable to create scene set layout\n";
        return false;
    }

    layout_info.bindingCount = 1;
    layout_info.pBindings = &model_binding;
    if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr, &model_set_layout) !=
        VK_SUCCESS) {
        std::cerr << "unable to create model set layout\n";
        return false;
    }

    const VkDescriptorSetLayout layouts_for_pipeline[] = {scene_set_layout, model_set_layout};

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 2;
    pipeline_layout_info.pSetLayouts = layouts_for_pipeline;

    if (vkCreatePipelineLayout(context.device, &pipeline_layout_info, nullptr, &pipeline_layout) !=
        VK_SUCCESS) {
        std::cerr << "unable to create pipeline layout\n";
        return false;
    }

    constexpr uint32_t descriptor_set_capacity = 4096;
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, descriptor_set_capacity},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
    };

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = descriptor_set_capacity;
    pool_info.poolSizeCount = 2;
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

    return true;
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
    if (!ensure_model_capacity(model_count) ||
        (!frame.light_lines.empty() && !ensure_visualization_line_capacity(frame.light_lines.size())) ||
        !ensure_light_buffer<gpu::PointLight>(point_light_buffer, point_light_capacity,
                                              frame.point_lights.size(), gpu::point_lights_binding) ||
        !ensure_light_buffer<gpu::SpotLight>(spot_light_buffer, spot_light_capacity,
                                             frame.spot_lights.size(), gpu::spot_lights_binding)) {
        std::cerr << "unable to grow GPU data buffers\n";
        return false;
    }

    const auto scene = make_scene_uniforms(frame);
    const auto points = make_point_light_data(frame.point_lights);
    const auto spots = make_spot_light_data(frame.spot_lights);
    if (!write_buffer(scene_uniform_buffer, &scene, sizeof(scene)) ||
        !write_buffer(point_light_buffer, points.data(), points.size() * sizeof(gpu::PointLight)) ||
        !write_buffer(spot_light_buffer, spots.data(), spots.size() * sizeof(gpu::SpotLight))) {
        return false;
    }

    for (size_t i = 0; i < frame.objects.size(); ++i) {
        const auto object = make_object_uniforms(frame.objects[i]);
        if (!write_buffer(model_uniforms_buffers[i], &object, sizeof(object))) {
            return false;
        }
    }
    if (!frame.light_lines.empty()) {
        const size_t line_model_index = frame.objects.size();
        const gpu::ObjectUniforms line_object{};
        if (!write_buffer(model_uniforms_buffers[line_model_index], &line_object, sizeof(line_object)) ||
            !write_buffer(visualization_line_buffer, frame.light_lines.data(),
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
        const VkDescriptorSet sets[] = {scene_set, model_sets[i]};

        vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                0, 2, sets, 0, nullptr);
        vkCmdDrawIndexed(fd.command_buffer, mesh.index_count, 1, 0, 0, 0);
    }

    if (!frame.light_lines.empty()) {
        const size_t line_model_index = frame.objects.size();
        vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.lines);
        vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &visualization_line_buffer.buffer,
                               &vertex_offset);
        const VkDescriptorSet sets[] = {scene_set, model_sets[line_model_index]};
        vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                0, 2, sets, 0, nullptr);
        vkCmdDraw(fd.command_buffer, static_cast<uint32_t>(frame.light_lines.size()), 1, 0, 0);
    }

    vkCmdEndRenderPass(fd.command_buffer);

    if (vkEndCommandBuffer(fd.command_buffer) != VK_SUCCESS) {
        std::cerr << "unable to finish writing command buffer\n";
    }
}
}

bool initialize() {
    if (!create_descriptor_resources() ||
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
    for (GPUBuffer &buffer : model_uniforms_buffers) {
        destroy_buffer(buffer);
    }
    model_uniforms_buffers.clear();
    model_sets.clear();
    destroy_buffer(sphere_mesh.vertices);
    destroy_buffer(sphere_mesh.indices);
    destroy_buffer(plane_mesh.vertices);
    destroy_buffer(plane_mesh.indices);
    sphere_mesh = {};
    plane_mesh = {};
    destroy_buffer(visualization_line_buffer);

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
    if (!create_mapped_buffer(vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, replacement.vertices) ||
        !create_mapped_buffer(index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, replacement.indices) ||
        !write_buffer(replacement.vertices, data.vertices.data(), vertex_bytes) ||
        !write_buffer(replacement.indices, data.indices.data(), index_bytes)) {
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
