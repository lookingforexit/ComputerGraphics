#include "graphics.hpp"

#include <algorithm>
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
        GPUBuffer environment_vertex_buffer;
        GPUBuffer environment_index_buffer;
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
        VkPipeline pipeline;
        VkPipeline line_pipeline;

        uint32_t index_count;
        uint32_t environment_index_count;
        VkDeviceSize visualization_line_capacity = 0;

        bool createMappedBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GPUBuffer& result) {
            const auto& context = internal::context;

            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = size;
            buffer_info.usage = usage;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo allocation_info{};
            allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
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

        bool ensureVisualizationLineCapacity(size_t vertex_count) {
            const VkDeviceSize requested = vertex_count * sizeof(Vertex);
            if (requested <= visualization_line_capacity) return true;
            if (internal::context.graphics_queue != VK_NULL_HANDLE) {
                vkQueueWaitIdle(internal::context.graphics_queue);
            }
            GPUBuffer replacement{};
            const VkDeviceSize new_capacity = std::max<VkDeviceSize>(requested, visualization_line_capacity * 2);
            if (!createMappedBuffer(new_capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, replacement)) return false;
            destroyBuffer(visualization_line_buffer);
            visualization_line_buffer = replacement;
            visualization_line_capacity = new_capacity;
            return true;
        }

        bool ensureLightBuffer(GPUBuffer& buffer, VkDeviceSize& capacity, VkDeviceSize requested,
                               uint32_t binding) {
            requested = std::max<VkDeviceSize>(requested, 1);
            if (requested <= capacity) return true;

            // Buffer replacement invalidates its descriptor, so wait for the prior frame first.
            if (internal::context.graphics_queue != VK_NULL_HANDLE) {
                vkQueueWaitIdle(internal::context.graphics_queue);
            }
            const VkDeviceSize new_capacity = std::max(requested, capacity * 2);
            GPUBuffer replacement{};
            if (!createMappedBuffer(new_capacity * (binding == 1 ? sizeof(PointLight) : sizeof(SpotLight)),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, replacement)) {
                return false;
            }
            destroyBuffer(buffer);
            buffer = replacement;
            capacity = new_capacity;

            VkDescriptorBufferInfo info{};
            info.buffer = buffer.buffer;
            info.range = capacity * (binding == 1 ? sizeof(PointLight) : sizeof(SpotLight));
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

        bool ensureModelCapacity(size_t count) {
            const auto& context = internal::context;
            while (model_uniforms_buffers.size() < count) {
                GPUBuffer buffer{};
                if (!createMappedBuffer(sizeof(ModelUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, buffer)) {
                    return false;
                }

                VkDescriptorSet set = VK_NULL_HANDLE;
                VkDescriptorSetAllocateInfo allocate{};
                allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocate.descriptorPool = descriptor_pool;
                allocate.descriptorSetCount = 1;
                allocate.pSetLayouts = &model_set_layout;
                if (vkAllocateDescriptorSets(context.device, &allocate, &set) != VK_SUCCESS) {
                    destroyBuffer(buffer);
                    std::cerr << "unable to allocate a model descriptor set\n";
                    return false;
                }

                VkDescriptorBufferInfo info{buffer.buffer, 0, sizeof(ModelUniforms)};
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

            const VkDescriptorSetLayoutBinding scene_bindings[] = {
                {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            };
            const VkDescriptorSetLayoutBinding model_binding = {
                0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr
            };

            VkDescriptorSetLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_info.bindingCount = 3;
            layout_info.pBindings = scene_bindings;

            if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr, &scene_set_layout) != VK_SUCCESS) {
                std::cerr << "unable to create scene set layout\n";
                return false;
            }

            layout_info.bindingCount = 1;
            layout_info.pBindings = &model_binding;
            if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr, &model_set_layout) != VK_SUCCESS) {
                std::cerr << "unable to create model set layout\n";
                return false;
            }

            const VkDescriptorSetLayout layouts_for_pipeline[] = {
                scene_set_layout,
                model_set_layout
            };

            VkPipelineLayoutCreateInfo pipeline_layout_info{};
            pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipeline_layout_info.setLayoutCount = 2;
            pipeline_layout_info.pSetLayouts = layouts_for_pipeline;

            if (vkCreatePipelineLayout(context.device, &pipeline_layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
                std::cerr << "unable to create pipeline layout\n";
                return false;
            }

            const VkDescriptorPoolSize pool_sizes[] = {
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
            };

            VkDescriptorPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            pool_info.maxSets = 4096;
            pool_info.poolSizeCount = 2;
            pool_info.pPoolSizes = pool_sizes;

            if (vkCreateDescriptorPool(context.device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS) {
                std::cerr << "unable to create descriptor pool\n";
                return false;
            }

            if (!createMappedBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, scene_uniform_buffer) ||
                !createMappedBuffer(sizeof(PointLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, point_light_buffer) ||
                !createMappedBuffer(sizeof(SpotLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, spot_light_buffer)) {
                return false;
            }

            model_uniforms_buffers.resize(kMaxObjectCount);
            model_sets.resize(kMaxObjectCount);
            for (GPUBuffer& buffer : model_uniforms_buffers) {
                if (!createMappedBuffer(sizeof(ModelUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, buffer)) {
                    return false;
                }
            }

            std::array<VkDescriptorSetLayout, kMaxObjectCount + 1> set_layouts{};
            set_layouts[0] = scene_set_layout;

            for (uint32_t i = 0; i < kMaxObjectCount; ++i) {
                set_layouts[i + 1] = model_set_layout;
            }

            std::array<VkDescriptorSet, kMaxObjectCount + 1> sets{};

            VkDescriptorSetAllocateInfo allocate_info{};
            allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate_info.descriptorPool = descriptor_pool;
            allocate_info.descriptorSetCount = static_cast<uint32_t>(set_layouts.size());
            allocate_info.pSetLayouts = set_layouts.data();

            if (vkAllocateDescriptorSets(context.device, &allocate_info, sets.data()) != VK_SUCCESS) {
                std::cerr << "unable to allocate descriptor sets\n";
                return false;
            }

            scene_set = sets[0];

            for (uint32_t i = 0; i < kMaxObjectCount; ++i) {
                model_sets[i] = sets[i + 1];
            }

            std::array<VkDescriptorBufferInfo, kMaxObjectCount + 3> buffer_infos{};
            std::array<VkWriteDescriptorSet, kMaxObjectCount + 3> writes{};

            buffer_infos[0] = {scene_uniform_buffer.buffer, 0, sizeof(SceneUniforms)};
            buffer_infos[1] = {point_light_buffer.buffer, 0, sizeof(PointLight)};
            buffer_infos[2] = {spot_light_buffer.buffer, 0, sizeof(SpotLight)};
            for (uint32_t binding = 0; binding < 3; ++binding) {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = scene_set;
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = binding == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[binding].pBufferInfo = &buffer_infos[binding];
            }

            for (uint32_t i = 0; i < kMaxObjectCount; ++i) {
                buffer_infos[i + 3] = {model_uniforms_buffers[i].buffer, 0, sizeof(ModelUniforms)};

                writes[i + 3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i + 3].dstSet = model_sets[i];
                writes[i + 3].dstBinding = 0;
                writes[i + 3].descriptorCount = 1;
                writes[i + 3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[i + 3].pBufferInfo = &buffer_infos[i + 3];
            }

            vkUpdateDescriptorSets(context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

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

            VkVertexInputAttributeDescription attributes[3]{};

            attributes[0].location = 0;
            attributes[0].binding = 0;
            attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
            attributes[0].offset = offsetof(Vertex, position);

            attributes[1].location = 1;
            attributes[1].binding = 0;
            attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
            attributes[1].offset = offsetof(Vertex, normal);

            attributes[2].location = 2;
            attributes[2].binding = 0;
            attributes[2].format = VK_FORMAT_R32G32B32_SFLOAT;
            attributes[2].offset = offsetof(Vertex, color);

            VkPipelineVertexInputStateCreateInfo vertex_input{};
            vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &vertex_binding;
            vertex_input.vertexAttributeDescriptionCount = 3;
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

            const VkResult status = vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1,
                                                               &pipeline_info, nullptr, &pipeline);
            if (status == VK_SUCCESS) {
                input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                pipeline_info.pInputAssemblyState = &input_assembly;
                const VkResult line_status = vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1,
                                                                        &pipeline_info, nullptr, &line_pipeline);
                if (line_status != VK_SUCCESS) {
                    std::cerr << "unable to create light visualization pipeline\n";
                    vkDestroyPipeline(context.device, pipeline, nullptr);
                    pipeline = VK_NULL_HANDLE;
                    vkDestroyShaderModule(context.device, vertex_shader, nullptr);
                    vkDestroyShaderModule(context.device, fragment_shader, nullptr);
                    return false;
                }
            }

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
        if (line_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(context.device, line_pipeline, nullptr);
            line_pipeline = VK_NULL_HANDLE;
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
        destroyBuffer(point_light_buffer);
        destroyBuffer(spot_light_buffer);
        for (GPUBuffer& buffer : model_uniforms_buffers) {
            destroyBuffer(buffer);
        }
        model_uniforms_buffers.clear();
        model_sets.clear();
        destroyBuffer(vertex_buffer);
        destroyBuffer(index_buffer);
        destroyBuffer(environment_vertex_buffer);
        destroyBuffer(environment_index_buffer);
        destroyBuffer(visualization_line_buffer);

        if (scene_set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context.device, scene_set_layout, nullptr);
            scene_set_layout = VK_NULL_HANDLE;
        }

        if (model_set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context.device, model_set_layout, nullptr);
            model_set_layout = VK_NULL_HANDLE;
        }

        scene_set = VK_NULL_HANDLE;
        index_count = 0;
        environment_index_count = 0;
        visualization_line_capacity = 0;
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

    bool getEnvironmentGeometry(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices) {
        if (vertices.empty() || indices.empty()) {
            std::cerr << "environment vertices or indices are empty\n";
            return false;
        }

        const VkDeviceSize vertex_bytes = vertices.size() * sizeof(Vertex);
        const VkDeviceSize index_bytes = indices.size() * sizeof(uint32_t);
        if (!createMappedBuffer(vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, environment_vertex_buffer) ||
            !writeBuffer(environment_vertex_buffer, vertices.data(), vertex_bytes)) {
            std::cerr << "failed to create environment vertex buffer\n";
            destroyBuffer(environment_vertex_buffer);
            return false;
        }
        if (!createMappedBuffer(index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, environment_index_buffer) ||
            !writeBuffer(environment_index_buffer, indices.data(), index_bytes)) {
            std::cerr << "failed to create environment index buffer\n";
            destroyBuffer(environment_vertex_buffer);
            destroyBuffer(environment_index_buffer);
            return false;
        }

        environment_index_count = static_cast<uint32_t>(indices.size());
        return true;
    }

    void render(const internal::FrameData &fd, const SceneUniforms &scene,
                const std::vector<ModelUniforms> &objects,
                const std::vector<PointLight> &point_lights,
                const std::vector<SpotLight> &spot_lights,
                const std::vector<Vertex> &visualization_lines) {
        if (fd.command_buffer == VK_NULL_HANDLE || fd.framebuffer == VK_NULL_HANDLE ||
            pipeline == VK_NULL_HANDLE || vertex_buffer.buffer == VK_NULL_HANDLE ||
            index_buffer.buffer == VK_NULL_HANDLE) {
            return;
        }

        const size_t line_model_index = objects.size();
        const size_t model_count = objects.size() + (visualization_lines.empty() ? 0 : 1);
        if (!ensureModelCapacity(model_count) ||
            (!visualization_lines.empty() && !ensureVisualizationLineCapacity(visualization_lines.size())) ||
            !ensureLightBuffer(point_light_buffer, point_light_capacity, point_lights.size(), 1) ||
            !ensureLightBuffer(spot_light_buffer, spot_light_capacity, spot_lights.size(), 2)) {
            std::cerr << "unable to grow GPU data buffers\n";
            return;
        }

        const auto& context = internal::context;

        if (!writeBuffer(scene_uniform_buffer, &scene, sizeof(SceneUniforms))) return;

        const PointLight empty_point{};
        const SpotLight empty_spot{};
        const void* point_data = point_lights.empty() ? static_cast<const void*>(&empty_point) : point_lights.data();
        const void* spot_data = spot_lights.empty() ? static_cast<const void*>(&empty_spot) : spot_lights.data();
        const VkDeviceSize point_bytes = std::max<VkDeviceSize>(point_lights.size(), 1) * sizeof(PointLight);
        const VkDeviceSize spot_bytes = std::max<VkDeviceSize>(spot_lights.size(), 1) * sizeof(SpotLight);
        if (!writeBuffer(point_light_buffer, point_data, point_bytes) ||
            !writeBuffer(spot_light_buffer, spot_data, spot_bytes)) return;

        for (size_t i = 0; i < objects.size(); ++i) {
            if (!writeBuffer(model_uniforms_buffers[i], &objects[i], sizeof(ModelUniforms))) return;
        }
        if (!visualization_lines.empty()) {
            ModelUniforms line_model{};
            line_model.model[0][0] = line_model.model[1][1] = line_model.model[2][2] = line_model.model[3][3] = 1.0f;
            line_model.albedo[0] = line_model.albedo[1] = line_model.albedo[2] = line_model.albedo[3] = 1.0f;
            line_model.material_parameters[1] = 1.0f;
            line_model.material_parameters[2] = 1.0f;
            if (!writeBuffer(model_uniforms_buffers[line_model_index], &line_model, sizeof(ModelUniforms)) ||
                !writeBuffer(visualization_line_buffer, visualization_lines.data(),
                             visualization_lines.size() * sizeof(Vertex))) return;
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

        constexpr VkDeviceSize vertex_offset = 0;

        for (uint32_t i = 0; i < objects.size(); ++i) {
            const bool is_environment = objects[i].material_parameters[3] > 0.5f;
            const VkBuffer geometry_buffer = is_environment ? environment_vertex_buffer.buffer : vertex_buffer.buffer;
            const VkBuffer geometry_indices = is_environment ? environment_index_buffer.buffer : index_buffer.buffer;
            const uint32_t geometry_index_count = is_environment ? environment_index_count : index_count;
            if (geometry_buffer == VK_NULL_HANDLE || geometry_indices == VK_NULL_HANDLE) continue;

            vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &geometry_buffer, &vertex_offset);
            vkCmdBindIndexBuffer(fd.command_buffer, geometry_indices, 0, VK_INDEX_TYPE_UINT32);
            const VkDescriptorSet sets[] = {
                scene_set,
                model_sets[i]
            };

            vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2, sets, 0, nullptr);
            vkCmdDrawIndexed(fd.command_buffer, geometry_index_count, 1, 0, 0, 0);
        }

        if (!visualization_lines.empty() && line_pipeline != VK_NULL_HANDLE) {
            vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline);
            vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &visualization_line_buffer.buffer, &vertex_offset);
            const VkDescriptorSet sets[] = {scene_set, model_sets[line_model_index]};
            vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2,
                                    sets, 0, nullptr);
            vkCmdDraw(fd.command_buffer, static_cast<uint32_t>(visualization_lines.size()), 1, 0, 0);
        }

        vkCmdEndRenderPass(fd.command_buffer);

        if (vkEndCommandBuffer(fd.command_buffer) != VK_SUCCESS) {
            std::cerr << "unable to finish writing command buffer\n";
        }
    }
}
