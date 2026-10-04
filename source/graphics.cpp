#include "graphics.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>

namespace graphics::core {
    namespace {
        struct GPUBuffer {
            VkBuffer buffer;
            VmaAllocation allocation;
            void* mapped;
        };

        GPUBuffer vertex_buffer;
        GPUBuffer index_buffer;
        GPUBuffer scene_uniform_buffer;
        std::array<GPUBuffer, kMaxObjectCount> model_uniforms_buffers;

        VkDescriptorSetLayout scene_set_layout;
        VkDescriptorSetLayout model_set_layout;
        VkDescriptorPool descriptor_pool;

        VkDescriptorSet scene_set;
        std::array<VkDescriptorSet, kMaxObjectCount> model_sets;

        VkPipelineLayout pipeline_layout;
        VkPipeline pipeline;

        uint32_t index_count;

        bool createMappedBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GPUBuffer& result) {
            const auto& context = internal::context;

            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = size;
            buffer_info.usage = usage;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo allocation_info{};
            allocation_info.flags =
                VMA_ALLOCATION_CREATE_MAPPED_BIT |
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
            allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            allocation_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

            VmaAllocationInfo allocation_result{};

            const VkResult status = vmaCreateBuffer(
                context.allocator,
                &buffer_info,
                &allocation_info,
                &result.buffer,
                &result.allocation,
                &allocation_result
            );

            if (status != VK_SUCCESS) {
                std::cerr << "cant create vulkan buffer\n";
                return false;
            }

            result.mapped = allocation_result.pMappedData;
            return true;
        }

        void destroyBuffer(GPUBuffer& buffer) {
            if (buffer.buffer == VK_NULL_HANDLE) {
                return;
            }

            const auto& context = internal::context;

            vmaDestroyBuffer(context.allocator, buffer.buffer, buffer.allocation);
            buffer.buffer = VK_NULL_HANDLE;
        }

        bool writeBuffer(GPUBuffer& buffer, const void* data, VkDeviceSize size) {
            if (buffer.mapped == nullptr) {
                return false;
            }

            std::memcpy(buffer.mapped, data, size);

            return vmaFlushAllocation(internal::context.allocator, buffer.allocation, 0, size) == VK_SUCCESS;
        }

        VkShaderModule loadShader(const char* path) {
            const auto& context = internal::context;

            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) {
                std::cerr << "unable to open shader\n";
                return VK_NULL_HANDLE;
            }

            const std::streamsize file_size = file.tellg();
            if (file_size <= 0) {
                std::cerr << "incorrect size of shader\n";
                return VK_NULL_HANDLE;
            }

            std::vector<uint32_t> code(file_size / sizeof(uint32_t));
            file.seekg(0);
            file.read(reinterpret_cast<char*>(code.data()), file_size);

            VkShaderModuleCreateInfo shader_info{};
            shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            shader_info.codeSize = file_size;
            shader_info.pCode = code.data();

            VkShaderModule shader{};
            if (vkCreateShaderModule(context.device, &shader_info, nullptr, &shader) != VK_SUCCESS) {
                std::cerr << "failed to create shader module\n";
                return VK_NULL_HANDLE;
            }

            return shader;
        }

        bool createDescriptorResources() {
            const auto& context = internal::context;

            VkDescriptorSetLayoutBinding binding{};
            binding.binding = 0;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

            VkDescriptorSetLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_info.bindingCount = 1;
            layout_info.pBindings = &binding;

            if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr, &scene_set_layout) != VK_SUCCESS) {
                std::cerr << "unable to create scene set layout\n";
                return false;
            }

            if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr, &model_set_layout) != VK_SUCCESS) {
                std::cerr << "unable to create model set layout\n";
                return false;
            }

            const VkDescriptorSetLayout layouts_for_pipeline[] = {
                scene_set_layout,
                model_set_layout
            };

            VkPipelineLayoutCreateInfo pipeline_layout_info{};
            pipeline_layout_info.sType =
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipeline_layout_info.setLayoutCount = 2;
            pipeline_layout_info.pSetLayouts = layouts_for_pipeline;

            if (vkCreatePipelineLayout(context.device, &pipeline_layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
                std::cerr << "unable to create pipeline layout\n";
                return false;
            }

            VkDescriptorPoolSize pool_size{};
            pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            pool_size.descriptorCount = 1 + kMaxObjectCount;

            VkDescriptorPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.maxSets = 1 + kMaxObjectCount;
            pool_info.poolSizeCount = 1;
            pool_info.pPoolSizes = &pool_size;

            if (vkCreateDescriptorPool(context.device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS) {
                std::cerr << "unable to create descriptor pool\n";
                return false;
            }

            if (!createMappedBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, scene_uniform_buffer)) {
                return false;
            }

            for (GPUBuffer& buffer : model_uniforms_buffers) {
                if (!createMappedBuffer(sizeof(ModelUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, buffer)) {
                    return false;
                }
            }

            std::array<VkDescriptorSetLayout, 1 + kMaxObjectCount> set_layouts{};
            set_layouts[0] = scene_set_layout;

            for (uint32_t i = 0; i < kMaxObjectCount; ++i) {
                set_layouts[1 + i] = model_set_layout;
            }

            std::array<VkDescriptorSet, 1 + kMaxObjectCount> sets{};

            VkDescriptorSetAllocateInfo allocate_info{};
            allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate_info.descriptorPool = descriptor_pool;
            allocate_info.descriptorSetCount = set_layouts.size();
            allocate_info.pSetLayouts = set_layouts.data();

            if (vkAllocateDescriptorSets(context.device, &allocate_info, sets.data()) != VK_SUCCESS) {
                std::cerr << "unable to allocate descriptor sets\n";
                return false;
            }

            scene_set = sets[0];

            for (uint32_t i = 0; i < kMaxObjectCount; ++i) {
                model_sets[i] = sets[1 + i];
            }

            std::array<VkDescriptorBufferInfo, 1 + kMaxObjectCount> buffer_infos{};

            std::array<VkWriteDescriptorSet, 1 + kMaxObjectCount> writes{};

            buffer_infos[0].buffer = scene_uniform_buffer.buffer;
            buffer_infos[0].offset = 0;
            buffer_infos[0].range = sizeof(SceneUniforms);

            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = scene_set;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &buffer_infos[0];

            for (uint32_t i = 0; i < kMaxObjectCount; ++i) {
                buffer_infos[1 + i].buffer = model_uniforms_buffers[i].buffer;
                buffer_infos[1 + i].offset = 0;
                buffer_infos[1 + i].range = sizeof(ModelUniforms);

                writes[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1 + i].dstSet = model_sets[i];
                writes[1 + i].dstBinding = 0;
                writes[1 + i].descriptorCount = 1;
                writes[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[1 + i].pBufferInfo = &buffer_infos[1 + i];
            }

            vkUpdateDescriptorSets(context.device,writes.size(),writes.data(),0,nullptr);

            return true;
        }

        bool createPipeline() {
            const auto& context = internal::context;

            VkShaderModule vertex_shader = loadShader("shaders/shader.vert.spv");
            VkShaderModule fragment_shader = loadShader("shaders/shader.frag.spv");

             if (vertex_shader == VK_NULL_HANDLE || fragment_shader == VK_NULL_HANDLE) {
                if (vertex_shader != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(context.device, vertex_shader, nullptr);
                }

                if (fragment_shader != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(context.device, fragment_shader, nullptr);
                }

                return false;
            }

            VkPipelineShaderStageCreateInfo shader_stages[2]{};

            shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            shader_stages[0].module = vertex_shader;
            shader_stages[0].pName = "main";

            shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            shader_stages[1].module = fragment_shader;
            shader_stages[1].pName = "main";

            VkVertexInputBindingDescription vertex_binding{};
            vertex_binding.binding = 0;
            vertex_binding.stride = sizeof(Vertex);
            vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            VkVertexInputAttributeDescription attributes[2]{};

            attributes[0].location = 0;
            attributes[0].binding = 0;
            attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
            attributes[0].offset = offsetof(Vertex, position);

            attributes[1].location = 1;
            attributes[1].binding = 0;
            attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
            attributes[1].offset = offsetof(Vertex, color);

            VkPipelineVertexInputStateCreateInfo vertex_input{};
            vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &vertex_binding;
            vertex_input.vertexAttributeDescriptionCount = 2;
            vertex_input.pVertexAttributeDescriptions = attributes;

            VkPipelineInputAssemblyStateCreateInfo input_assembly{};
            input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo viewport_state{};
            viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewport_state.viewportCount = 1;
            viewport_state.scissorCount = 1;

            VkDynamicState dynamic_states[] = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR
            };

            VkPipelineDynamicStateCreateInfo dynamic_state{};
            dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic_state.dynamicStateCount = 2;
            dynamic_state.pDynamicStates = dynamic_states;

            VkPipelineRasterizationStateCreateInfo rasterization{};
            rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterization.polygonMode = VK_POLYGON_MODE_FILL;

            rasterization.cullMode = VK_CULL_MODE_NONE;
            rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
            rasterization.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo depth_state{};
            depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depth_state.depthTestEnable = VK_TRUE;
            depth_state.depthWriteEnable = VK_TRUE;
            depth_state.depthCompareOp = VK_COMPARE_OP_LESS;

            VkPipelineColorBlendAttachmentState blend_attachment{};
            blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            VkPipelineColorBlendStateCreateInfo blend_state{};
            blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            blend_state.attachmentCount = 1;
            blend_state.pAttachments = &blend_attachment;

            VkGraphicsPipelineCreateInfo pipeline_info{};
            pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipeline_info.stageCount = 2;
            pipeline_info.pStages = shader_stages;
            pipeline_info.pVertexInputState = &vertex_input;
            pipeline_info.pInputAssemblyState = &input_assembly;
            pipeline_info.pViewportState = &viewport_state;
            pipeline_info.pRasterizationState = &rasterization;
            pipeline_info.pMultisampleState = &multisampling;
            pipeline_info.pDepthStencilState = &depth_state;
            pipeline_info.pColorBlendState = &blend_state;
            pipeline_info.pDynamicState = &dynamic_state;
            pipeline_info.layout = pipeline_layout;

            pipeline_info.renderPass = context.render_pass;
            pipeline_info.subpass = 0;
            pipeline_info.basePipelineIndex = -1;

            const VkResult status = vkCreateGraphicsPipelines(context.device,VK_NULL_HANDLE,1, &pipeline_info,nullptr, &pipeline);

            vkDestroyShaderModule(context.device, vertex_shader, nullptr);
            vkDestroyShaderModule(context.device, fragment_shader, nullptr);

            if (status != VK_SUCCESS) {
                std::cerr << "unable to create graphics pipeline\n";
                return false;
            }

            return true;
        }
    }

    bool initialize() {
        if (!createDescriptorResources() || !createPipeline()) {
            shutdown();
            return false;
        }

        return true;
    }

    void shutdown() {
        const auto& context = internal::context;

        if (context.device == VK_NULL_HANDLE) {
            return;
        }

        if (context.graphics_queue != VK_NULL_HANDLE) {
            vkQueueWaitIdle(context.graphics_queue);
        }

        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(context.device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }

        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
            pipeline_layout = VK_NULL_HANDLE;
        }

        if (descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(context.device, descriptor_pool, nullptr);
            descriptor_pool = VK_NULL_HANDLE;
        }

        destroyBuffer(scene_uniform_buffer);
        for (GPUBuffer& buffer : model_uniforms_buffers) {
            destroyBuffer(buffer);
        }
        destroyBuffer(vertex_buffer);
        destroyBuffer(index_buffer);

        if (scene_set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context.device, scene_set_layout, nullptr);
            scene_set_layout = VK_NULL_HANDLE;
        }

        if (model_set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context.device, model_set_layout, nullptr);
            model_set_layout = VK_NULL_HANDLE;
        }

        scene_set = VK_NULL_HANDLE;
        model_sets.fill(VK_NULL_HANDLE);
        index_count = 0;
    }

    bool getGeometry(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices) {
        if (vertices.empty() || indices.empty()) {
            std::cerr << "vertices or indices are empty\n";
            return false;
        }

        const VkDeviceSize vertex_bytes = vertices.size() * sizeof(Vertex);
        const VkDeviceSize indices_bytes = indices.size() * sizeof(uint32_t);

        if (!createMappedBuffer(vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer)) {
            std::cerr << "failed to create vertex buffer\n";
            return false;
        }
        if (!writeBuffer(vertex_buffer, vertices.data(), vertex_bytes)) {
            std::cerr << "failed to write vertex buffer\n";
            destroyBuffer(vertex_buffer);
            return false;
        }

        if (!createMappedBuffer(indices_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer)) {
            std::cerr << "failed to create index buffer\n";
            return false;
        }
        if (!writeBuffer(index_buffer, indices.data(), indices_bytes)) {
            std::cerr << "failed to write index buffer\n";
            destroyBuffer(index_buffer);
            return false;
        }

        index_count = indices.size();

        return true;
    }

    void render(const internal::FrameData &fd, const SceneUniforms &scene, const std::vector<ModelUniforms> &objects) {
        if (fd.command_buffer == VK_NULL_HANDLE || fd.framebuffer == VK_NULL_HANDLE ||
            pipeline == VK_NULL_HANDLE || vertex_buffer.buffer == VK_NULL_HANDLE ||
            index_buffer.buffer == VK_NULL_HANDLE) {
            return;
        }

        if (objects.size() > kMaxObjectCount) {
            std::cerr << "too many objects\n";
            return;
        }

        const auto& context = internal::context;

        writeBuffer(scene_uniform_buffer, &scene, sizeof(SceneUniforms));

        for (size_t i = 0; i < objects.size(); ++i) {
            writeBuffer(model_uniforms_buffers[i], &objects[i], sizeof(ModelUniforms));
        }

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

        vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        constexpr VkDeviceSize vertex_offset = 0;

        vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertex_buffer.buffer, &vertex_offset);

        vkCmdBindIndexBuffer(fd.command_buffer, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        for (uint32_t i = 0; i < objects.size(); ++i) {
            const VkDescriptorSet sets[] = {
                scene_set,
                model_sets[i]
            };

            vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2, sets, 0, nullptr);

            vkCmdDrawIndexed(fd.command_buffer,index_count, 1, 0, 0, 0);
        }

        vkCmdEndRenderPass(fd.command_buffer);

        if (vkEndCommandBuffer(fd.command_buffer) != VK_SUCCESS) {
            std::cerr << "unable to finish writing command buffer\n";
        }
    }
}
