/*
 * vkrunner
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include "vr-pipeline.h"
#include "vr-subprocess.h"
#include "vr-util.h"
#include "vr-script.h"
#include "vr-error-message.h"
#include "vr-buffer.h"
#include "vr-temp-file.h"

#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#ifndef WIN32
#include <unistd.h>
#endif

struct desc_set_bindings_info {
        VkDescriptorSetLayoutBinding *bindings;
        unsigned n_bindings;
        unsigned desc_set;
};

static const char *
stage_names[VR_SCRIPT_N_STAGES] = {
        [VR_SCRIPT_SHADER_STAGE_VERTEX] = "vert",
        [VR_SCRIPT_SHADER_STAGE_TESS_CTRL] = "tesc",
        [VR_SCRIPT_SHADER_STAGE_TESS_EVAL] = "tese",
        [VR_SCRIPT_SHADER_STAGE_GEOMETRY] = "geom",
        [VR_SCRIPT_SHADER_STAGE_FRAGMENT] = "frag",
        [VR_SCRIPT_SHADER_STAGE_COMPUTE] = "comp",
};

static const VkViewport
base_viewports[] = {
        {
                .width = VR_WINDOW_WIDTH,
                .height = VR_WINDOW_HEIGHT,
                .minDepth = 0.0f,
                .maxDepth = 1.0f
        }
};

static const VkRect2D
base_scissors[] = {
        {
                .extent = { VR_WINDOW_WIDTH, VR_WINDOW_HEIGHT }
        }
};

static const VkPipelineViewportStateCreateInfo
base_viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = VR_N_ELEMENTS(base_viewports),
        .pViewports = base_viewports,
        .scissorCount = VR_N_ELEMENTS(base_scissors),
        .pScissors = base_scissors
};

static const VkPipelineMultisampleStateCreateInfo
base_multisample_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
};

static char *
create_file_for_shader(const struct vr_config *config,
                       const struct vr_script_shader *shader)
{
        char *filename;
        FILE *out;

        if (!vr_temp_file_create_named(config, &out, &filename))
                return NULL;

        fwrite(shader->source, 1, shader->length, out);

        fclose(out);

        return filename;
}

static bool
load_stream_contents(const struct vr_config *config,
                     FILE *stream,
                     uint8_t **contents_out,
                     size_t *size_out)
{
        size_t got;
        long pos;

        fseek(stream, 0, SEEK_END);
        pos = ftell(stream);

        if (pos == -1) {
                vr_error_message(config, "ftell failed");
                return false;
        }

        size_t size = pos;
        rewind(stream);
        uint8_t *contents = vr_alloc(size);

        got = fread(contents, 1, size, stream);
        if (got != size) {
                vr_error_message(config, "Error reading file contents");
                vr_free(contents);
                return false;
        }

        *contents_out = contents;
        *size_out = size;

        return true;
}

static bool
show_disassembly(const struct vr_config *config,
                 const char *filename)
{
        char *args[] = {
                getenv("PIGLIT_SPIRV_DIS_BINARY"),
                (char *) filename,
                NULL
        };

        if (args[0] == NULL)
                args[0] = "spirv-dis";

        return vr_subprocess_command(config, args);
}

static VkShaderModule
compile_stage(const struct vr_config *config,
              struct vr_window *window,
              const struct vr_script *script,
              enum vr_script_shader_stage stage)
{
        struct vr_vk *vkfn = &window->vkfn;
        const int n_base_args = 6;
        int n_shaders = vr_list_length(&script->stages[stage]);
        char **args = alloca((n_base_args + n_shaders + 1) * sizeof args[0]);
        const struct vr_script_shader *shader;
        VkShaderModule module = VK_NULL_HANDLE;
        FILE *module_stream = NULL;
        char *module_filename;
        uint8_t *module_binary = NULL;
        size_t module_size;
        bool res;
        int i;

        memset(args + n_base_args, 0, (n_shaders + 1) * sizeof args[0]);

        if (!vr_temp_file_create_named(config,
                                       &module_stream,
                                       &module_filename))
                goto out;

        args[0] = getenv("PIGLIT_GLSLANG_VALIDATOR_BINARY");
        if (args[0] == NULL)
                args[0] = "glslangValidator";

        args[1] = "-V";
        args[2] = "-S";
        args[3] = (char *) stage_names[stage];
        args[4] = "-o";
        args[5] = module_filename;

        i = n_base_args;
        vr_list_for_each(shader, &script->stages[stage], link) {
                args[i] = create_file_for_shader(config, shader);
                if (args[i] == 0)
                        goto out;
                i++;
        }

        res = vr_subprocess_command(config, args);
        if (!res) {
                vr_error_message(config, "glslangValidator failed");
                goto out;
        }

        if (config->show_disassembly)
                show_disassembly(config, module_filename);

        if (!load_stream_contents(config,
                                  module_stream,
                                  &module_binary,
                                  &module_size))
                goto out;

        VkShaderModuleCreateInfo shader_module_create_info = {
                        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                        .codeSize = module_size,
                        .pCode = (const uint32_t *) module_binary
        };
        res = vkfn->vkCreateShaderModule(window->device,
                                         &shader_module_create_info,
                                         NULL, /* allocator */
                                         &module);
        if (res != VK_SUCCESS) {
                vr_error_message(config, "vkCreateShaderModule failed");
                module = VK_NULL_HANDLE;
                goto out;
        }

out:
        for (i = 0; i < n_shaders; i++) {
                if (args[i + n_base_args]) {
                        unlink(args[i + n_base_args]);
                        vr_free(args[i + n_base_args]);
                }
        }

        if (module_stream) {
                fclose(module_stream);
                unlink(module_filename);
                vr_free(module_filename);
        }

        if (module_binary)
                vr_free(module_binary);

        return module;
}

static VkShaderModule
assemble_stage(const struct vr_config *config,
               struct vr_window *window,
               const struct vr_script_shader *shader)
{
        struct vr_vk *vkfn = &window->vkfn;
        FILE *module_stream = NULL;
        char *module_filename;
        char *source_filename = NULL;
        uint8_t *module_binary = NULL;
        VkShaderModule module = VK_NULL_HANDLE;
        size_t module_size;
        bool res;

        if (!vr_temp_file_create_named(config,
                                       &module_stream,
                                       &module_filename))
                goto out;

        source_filename = create_file_for_shader(config, shader);
        if (source_filename == NULL)
                goto out;

        char *args[] = {
                getenv("PIGLIT_SPIRV_AS_BINARY"),
                "-o", module_filename,
                source_filename,
                NULL
        };

        if (args[0] == NULL)
                args[0] = "spirv-as";

        res = vr_subprocess_command(config, args);
        if (!res) {
                vr_error_message(config, "spirv-as failed");
                goto out;
        }

        if (config->show_disassembly)
                show_disassembly(config, module_filename);

        if (!load_stream_contents(config,
                                  module_stream,
                                  &module_binary,
                                  &module_size))
                goto out;

        VkShaderModuleCreateInfo shader_module_create_info = {
                        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                        .codeSize = module_size,
                        .pCode = (const uint32_t *) module_binary
        };
        res = vkfn->vkCreateShaderModule(window->device,
                                         &shader_module_create_info,
                                         NULL, /* allocator */
                                         &module);
        if (res != VK_SUCCESS) {
                vr_error_message(config, "vkCreateShaderModule failed");
                module = VK_NULL_HANDLE;
                goto out;
        }

out:
        if (source_filename) {
                unlink(source_filename);
                vr_free(source_filename);
        }

        if (module_stream) {
                fclose(module_stream);
                unlink(module_filename);
                vr_free(module_filename);
        }

        if (module_binary)
                vr_free(module_binary);

        return module;
}

static VkShaderModule
load_binary_stage(const struct vr_config *config,
                  struct vr_window *window,
                  const struct vr_script_shader *shader)
{
        struct vr_vk *vkfn = &window->vkfn;
        VkShaderModule module = VK_NULL_HANDLE;
        bool res;

        if (config->show_disassembly) {
                FILE *module_stream;
                char *module_filename;

                if (vr_temp_file_create_named(config,
                                              &module_stream,
                                              &module_filename)) {
                        fwrite(shader->source,
                               1, shader->length,
                               module_stream);
                        fclose(module_stream);

                        show_disassembly(config, module_filename);

                        unlink(module_filename);
                        vr_free(module_filename);
                }
        }

        VkShaderModuleCreateInfo shader_module_create_info = {
                        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                        .codeSize = shader->length,
                        .pCode = (const uint32_t *) shader->source
        };
        res = vkfn->vkCreateShaderModule(window->device,
                                         &shader_module_create_info,
                                         NULL, /* allocator */
                                         &module);
        if (res != VK_SUCCESS)
                vr_error_message(config, "vkCreateShaderModule failed");

        return module;
}

static VkShaderModule
build_stage(const struct vr_config *config,
            struct vr_window *window,
            const struct vr_script *script,
            enum vr_script_shader_stage stage)
{
        assert(!vr_list_empty(&script->stages[stage]));

        struct vr_script_shader *shader =
                vr_container_of(script->stages[stage].next,
                                struct vr_script_shader,
                                link);

        switch (shader->source_type) {
        case VR_SCRIPT_SOURCE_TYPE_GLSL:
                return compile_stage(config, window, script, stage);
        case VR_SCRIPT_SOURCE_TYPE_SPIRV:
                return assemble_stage(config, window, shader);
        case VR_SCRIPT_SOURCE_TYPE_BINARY:
                return load_binary_stage(config, window, shader);
        }

        vr_fatal("should not be reached");
}

static void
set_vertex_input_state(const struct vr_script *script,
                       VkPipelineVertexInputStateCreateInfo *state,
                       const struct vr_pipeline_key *key)
{
        memset(state, 0, sizeof *state);

        state->sType =
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        if (key->source == VR_PIPELINE_KEY_SOURCE_VERTEX_DATA &&
            script->vertex_data == NULL)
                return;

        VkVertexInputBindingDescription *input_binding =
                vr_calloc(sizeof *input_binding);

        input_binding[0].binding = 0;
        input_binding[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        state->vertexBindingDescriptionCount = 1;
        state->pVertexBindingDescriptions = input_binding;

        if (key->source == VR_PIPELINE_KEY_SOURCE_RECTANGLE) {
                VkVertexInputAttributeDescription *attrib =
                        vr_calloc(sizeof *attrib);
                state->vertexAttributeDescriptionCount = 1;
                state->pVertexAttributeDescriptions = attrib;

                input_binding[0].stride =
                        sizeof (struct vr_pipeline_vertex);

                attrib->location = 0;
                attrib->binding = 0;
                attrib->format = VK_FORMAT_R32G32B32_SFLOAT;
                attrib->offset = offsetof(struct vr_pipeline_vertex, x);

                return;
        }

        int n_attribs = vr_list_length(&script->vertex_data->attribs);
        VkVertexInputAttributeDescription *attrib_desc =
                vr_calloc((sizeof *attrib_desc) * n_attribs);
        const struct vr_vbo_attrib *attrib;

        state->vertexAttributeDescriptionCount = n_attribs;
        state->pVertexAttributeDescriptions = attrib_desc;
        input_binding[0].stride = script->vertex_data->stride;

        vr_list_for_each(attrib, &script->vertex_data->attribs, link) {
                attrib_desc->location = attrib->location;
                attrib_desc->binding = 0;
                attrib_desc->format = attrib->format->vk_format,
                attrib_desc->offset = attrib->offset;
                attrib_desc++;
        };
}

static VkPipeline
create_vk_pipeline(struct vr_pipeline *pipeline,
                   const struct vr_script *script,
                   const struct vr_pipeline_key *key,
                   bool allow_derivatives,
                   VkPipeline parent_pipeline)
{
        struct vr_window *window = pipeline->window;
        struct vr_vk *vkfn = &window->vkfn;
        VkResult res;
        int num_stages = 0;

        VkPipelineShaderStageCreateInfo stages[VR_SCRIPT_N_STAGES];
        memset(&stages, 0, sizeof stages);

        for (int i = 0; i < VR_SCRIPT_N_STAGES; i++) {
                if (i == VR_SCRIPT_SHADER_STAGE_COMPUTE ||
                    pipeline->modules[i] == VK_NULL_HANDLE)
                        continue;
                stages[num_stages].sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[num_stages].stage = VK_SHADER_STAGE_VERTEX_BIT << i;
                stages[num_stages].module = pipeline->modules[i];
                stages[num_stages].pName = "main";
                num_stages++;
        }

        VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
                .sType =
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
        };

        VkPipelineRasterizationStateCreateInfo rasterization_state = {
                .sType =
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
        };

        VkPipelineVertexInputStateCreateInfo vertex_input_state;
        set_vertex_input_state(script, &vertex_input_state, key);

        VkPipelineTessellationStateCreateInfo tessellation_state = {
                .sType =
                VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO
        };

        VkPipelineColorBlendAttachmentState blend_attachments[] = {
                {
                        .blendEnable = false,
                }
        };

        VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
                .sType =
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        };

        VkPipelineColorBlendStateCreateInfo color_blend_state = {
                .sType =
                VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .attachmentCount = VR_N_ELEMENTS(blend_attachments),
                .pAttachments = blend_attachments
        };

        VkGraphicsPipelineCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .pViewportState = &base_viewport_state,
                .pRasterizationState = &rasterization_state,
                .pMultisampleState = &base_multisample_state,
                .pDepthStencilState = &depth_stencil_state,
                .pColorBlendState = &color_blend_state,
                .pTessellationState = &tessellation_state,
                .subpass = 0,
                .basePipelineHandle = parent_pipeline,
                .basePipelineIndex = -1,

                .stageCount = num_stages,
                .pStages = stages,
                .pVertexInputState = &vertex_input_state,
                .pInputAssemblyState = &input_assembly_state,
                .layout = pipeline->layout,
                .renderPass = window->render_pass[0],
        };

        vr_pipeline_key_to_create_info(key, &info);

        if (allow_derivatives)
                info.flags |= VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
        if (parent_pipeline)
                info.flags |= VK_PIPELINE_CREATE_DERIVATIVE_BIT;

        if (!(pipeline->stages & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)))
                info.pTessellationState = NULL;

        VkPipeline vk_pipeline;

        res = vkfn->vkCreateGraphicsPipelines(window->device,
                                              pipeline->pipeline_cache,
                                              1, /* nCreateInfos */
                                              &info,
                                              NULL, /* allocator */
                                              &vk_pipeline);

        vr_free((void *) vertex_input_state.pVertexBindingDescriptions);
        vr_free((void *) vertex_input_state.pVertexAttributeDescriptions);

        if (res != VK_SUCCESS) {
                vr_error_message(window->config, "Error creating VkPipeline");
                return VK_NULL_HANDLE;
        }

        return vk_pipeline;
}

static VkPipeline
create_compute_pipeline(struct vr_pipeline *pipeline)
{
        struct vr_window *window = pipeline->window;
        struct vr_vk *vkfn = &window->vkfn;
        VkResult res;

        VkComputePipelineCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,

                .stage = {
                        .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                        .module =
                        pipeline->modules[VR_SCRIPT_SHADER_STAGE_COMPUTE],
                        .pName = "main"
                },
                .layout = pipeline->layout,
                .basePipelineHandle = VK_NULL_HANDLE,
                .basePipelineIndex = -1,
        };

        VkPipeline vk_pipeline;

        res = vkfn->vkCreateComputePipelines(window->device,
                                             pipeline->pipeline_cache,
                                             1, /* nCreateInfos */
                                             &info,
                                             NULL, /* allocator */
                                             &vk_pipeline);

        if (res != VK_SUCCESS) {
                vr_error_message(window->config, "Error creating VkPipeline");
                return VK_NULL_HANDLE;
        }

        return vk_pipeline;
}

static size_t
get_push_constant_size(const struct vr_script *script)
{
        size_t max = 0;

        for (int i = 0; i < script->n_commands; i++) {
                const struct vr_script_command *command =
                        script->commands + i;
                if (command->op != VR_SCRIPT_OP_SET_PUSH_CONSTANT)
                        continue;

                size_t end = (command->set_push_constant.offset +
                              command->set_push_constant.size);

                if (end > max)
                        max = end;
        }

        return max;
}

static VkShaderStageFlags
get_script_stages(const struct vr_script *script)
{
        VkShaderStageFlags flags = 0;

        for (int i = 0; i < VR_SCRIPT_N_STAGES; i++) {
                if (!vr_list_empty(script->stages + i))
                        flags |= VK_SHADER_STAGE_VERTEX_BIT << i;
        }

        return flags;
}

static VkPipelineLayout
create_vk_layout(struct vr_pipeline *pipeline,
                 const struct vr_script *script)
{
        struct vr_vk *vkfn = &pipeline->window->vkfn;
        VkResult res;

        VkPushConstantRange push_constant_range = {
                .stageFlags = pipeline->stages,
                .offset = 0,
                .size = get_push_constant_size(script)
        };
        VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };

        if (push_constant_range.size > 0) {
                pipeline_layout_create_info.pushConstantRangeCount = 1;
                pipeline_layout_create_info.pPushConstantRanges =
                        &push_constant_range;
        }

        if (pipeline->descriptor_set_layout) {
                pipeline_layout_create_info.setLayoutCount =
                        pipeline->n_desc_sets;
                pipeline_layout_create_info.pSetLayouts =
                        pipeline->descriptor_set_layout;
        }

        VkPipelineLayout layout;
        res = vkfn->vkCreatePipelineLayout(pipeline->window->device,
                                           &pipeline_layout_create_info,
                                           NULL, /* allocator */
                                           &layout);
        if (res != VK_SUCCESS) {
                vr_error_message(pipeline->window->config,
                                 "Error creating pipeline layout");
                return VK_NULL_HANDLE;
        }

        return layout;
}

static bool
create_vk_descriptor_set_layout(struct vr_pipeline *pipeline,
                                const struct vr_script *script)
{
        struct vr_vk *vkfn = &pipeline->window->vkfn;
        VkResult res;
        bool ret = false;
        size_t n_resources = script->n_resources;
        VkDescriptorSetLayoutBinding *bindings =
                vr_calloc(sizeof (*bindings) * n_resources);
        unsigned prev_desc_set = script->resources[0].desc_set;

        struct desc_set_bindings_info *info =
                vr_alloc(sizeof *info * n_resources);
        size_t n_desc_sets = 0;
        info[n_desc_sets].desc_set = prev_desc_set;
        info[n_desc_sets].bindings = bindings;

        unsigned n_type[VR_SCRIPT_RESOURCE_N_TYPES] = {0};

        for (unsigned i = 0; i < n_resources; i++) {
                const struct vr_script_resource *resource =
                        script->resources + i;
                VkDescriptorType descriptor_type;
                for (unsigned j = 0; j < VR_SCRIPT_RESOURCE_N_TYPES; ++j) {
                        if (resource->type == j) {
                                descriptor_type =
                                        VK_DESCRIPTOR_TYPE_SAMPLER + j;
                                ++n_type[j];
                                goto found_type;
                        }
                }
                vr_fatal("Unexpected resource type");
        found_type:
                bindings[i].binding = resource->binding;
                bindings[i].descriptorType = descriptor_type;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = pipeline->stages;

                if (prev_desc_set != resource->desc_set) {
                        info[n_desc_sets].n_bindings =
                                &bindings[i] - info[n_desc_sets].bindings;
                        ++n_desc_sets;
                        info[n_desc_sets].desc_set = resource->desc_set;
                        info[n_desc_sets].bindings = &bindings[i];
                        prev_desc_set = resource->desc_set;
                }
        }
        info[n_desc_sets].n_bindings =
                &bindings[n_resources] - info[n_desc_sets].bindings;
        ++n_desc_sets;

        VkDescriptorPoolSize pool_sizes[VR_SCRIPT_RESOURCE_N_TYPES] = {0};
        uint32_t n_pool_sizes = 0;
        for (unsigned i = 0; i < VR_SCRIPT_RESOURCE_N_TYPES; ++i) {
                if (n_type[i]) {
                        pool_sizes[n_pool_sizes].type =
                                VK_DESCRIPTOR_TYPE_SAMPLER + i;
                        pool_sizes[n_pool_sizes].descriptorCount = n_type[i];
                        ++n_pool_sizes;
                }
        }

        VkDescriptorPoolCreateInfo descriptor_pool_create_info = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                .maxSets = n_desc_sets,
                .poolSizeCount = n_pool_sizes,
                .pPoolSizes = pool_sizes
        };

        pipeline->descriptor_set_layout = NULL;

        res = vkfn->vkCreateDescriptorPool(pipeline->window->device,
                                           &descriptor_pool_create_info,
                                           NULL, /* allocator */
                                           &pipeline->window->context->
                                           descriptor_pool);
        if (res != VK_SUCCESS) {
                vr_error_message(pipeline->window->config,
                                 "Error creating VkDescriptorPool");
                goto error;
        }

        VkDescriptorSetLayout *descriptor_set_layout =
                vr_alloc(sizeof(VkDescriptorSetLayout) * n_desc_sets);
        unsigned *first_sets = vr_alloc(sizeof(unsigned) * n_desc_sets);

        for (unsigned i = 0; i < n_desc_sets; i++) {
                VkDescriptorSetLayoutCreateInfo create_info = {
                        .sType =
                         VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                        .bindingCount = info[i].n_bindings,
                        .pBindings = info[i].bindings
                };

                res = vkfn->vkCreateDescriptorSetLayout(
                                pipeline->window->device,
                                &create_info,
                                NULL, /* allocator */
                                &descriptor_set_layout[i]);

                if (res != VK_SUCCESS) {
                        vr_error_message(pipeline->window->config,
                                         "Error creating descriptor set layout");
                        goto error;
                }
                first_sets[i] = info[i].desc_set;
        }

        pipeline->descriptor_set_layout =
                vr_memdup(descriptor_set_layout,
                          sizeof(VkDescriptorSetLayout) * n_desc_sets);
        pipeline->desc_sets = first_sets;
        pipeline->n_desc_sets = n_desc_sets;
        ret = true;

error:
        vr_free(bindings);
        vr_free(info);
        vr_free(descriptor_set_layout);
        return ret;
}

struct vr_pipeline *
vr_pipeline_create(const struct vr_config *config,
                   struct vr_window *window,
                   const struct vr_script *script)
{
        struct vr_vk *vkfn = &window->vkfn;
        VkResult res;
        struct vr_pipeline *pipeline = vr_calloc(sizeof *pipeline);

        pipeline->window = window;

        for (int i = 0; i < VR_SCRIPT_N_STAGES; i++) {
                if (vr_list_empty(&script->stages[i]))
                        continue;

                pipeline->modules[i] = build_stage(config, window, script, i);
                if (pipeline->modules[i] == VK_NULL_HANDLE)
                        goto error;
        }

        VkPipelineCacheCreateInfo pipeline_cache_create_info = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
        };
        res = vkfn->vkCreatePipelineCache(window->device,
                                          &pipeline_cache_create_info,
                                          NULL, /* allocator */
                                          &pipeline->pipeline_cache);
        if (res != VK_SUCCESS) {
                vr_error_message(config, "Error creating pipeline cache");
                goto error;
        }

        pipeline->stages = get_script_stages(script);

        if (script->n_resources > 0) {
                if (!create_vk_descriptor_set_layout(pipeline, script) ||
                    !pipeline->descriptor_set_layout ||
                    !pipeline->n_desc_sets) {
                        goto error;
                } else {
                        for (unsigned i = 0; i < pipeline->n_desc_sets; i++) {
                                if (pipeline->descriptor_set_layout[i] ==
                                    VK_NULL_HANDLE)
                                        goto error;
                        }
                }
        }

        pipeline->layout = create_vk_layout(pipeline, script);
        if (pipeline->layout == VK_NULL_HANDLE)
                goto error;

        if (pipeline->stages & ~VK_SHADER_STAGE_COMPUTE_BIT) {
                const struct vr_pipeline_key *keys;
                struct vr_pipeline_key base_key;

                if (script->n_pipeline_keys > 0) {
                        keys = script->pipeline_keys;
                        pipeline->n_pipelines = script->n_pipeline_keys;
                } else {
                        /* Always create at least one pipeline */
                        vr_pipeline_key_init(&base_key);
                        keys = &base_key;
                        pipeline->n_pipelines = 1;
                }

                pipeline->pipelines = vr_calloc(sizeof (VkPipeline) *
                                                pipeline->n_pipelines);

                bool use_derivatives = pipeline->n_pipelines > 1;

                for (int i = 0; i < pipeline->n_pipelines; i++) {
                        pipeline->pipelines[i] =
                                create_vk_pipeline(pipeline,
                                                   script,
                                                   keys + i,
                                                   i == 0 && use_derivatives,
                                                   pipeline->pipelines[0]);
                        if (pipeline->pipelines[i] == VK_NULL_HANDLE)
                                goto error;
                }
        }

        if (pipeline->modules[VR_SCRIPT_SHADER_STAGE_COMPUTE]) {
                pipeline->compute_pipeline =
                        create_compute_pipeline(pipeline);
                if (pipeline->compute_pipeline == VK_NULL_HANDLE)
                        goto error;
        }

        return pipeline;

error:
        vr_pipeline_free(pipeline);
        return NULL;
}

void
vr_pipeline_free(struct vr_pipeline *pipeline)
{
        struct vr_window *window = pipeline->window;
        struct vr_vk *vkfn = &window->vkfn;

        if (pipeline->compute_pipeline) {
                vkfn->vkDestroyPipeline(window->device,
                                        pipeline->compute_pipeline,
                                        NULL /* allocator */);
        }

        for (int i = 0; i < pipeline->n_pipelines; i++) {
                if (pipeline->pipelines[i]) {
                        vkfn->vkDestroyPipeline(window->device,
                                                pipeline->pipelines[i],
                                                NULL /* allocator */);
                }
        }
        vr_free(pipeline->pipelines);

        if (pipeline->pipeline_cache) {
                vkfn->vkDestroyPipelineCache(window->device,
                                             pipeline->pipeline_cache,
                                             NULL /* allocator */);
        }

        if (pipeline->layout) {
                vkfn->vkDestroyPipelineLayout(window->device,
                                              pipeline->layout,
                                              NULL /* allocator */);
        }

        if (pipeline->descriptor_set_layout) {
                for (unsigned i = 0; i < pipeline->n_desc_sets; i++) {
                        VkDescriptorSetLayout dsl =
                                pipeline->descriptor_set_layout[i];
                        vkfn->vkDestroyDescriptorSetLayout(
                                        window->device,
                                        dsl,
                                        NULL /* allocator */);
                }
                vr_free(pipeline->descriptor_set_layout);
                vr_free(pipeline->desc_sets);
        }

        if (window->context->descriptor_pool) {
                vkfn->vkDestroyDescriptorPool(window->device,
                                              window->context->descriptor_pool,
                                              NULL /* allocator */);
                window->context->descriptor_pool = VK_NULL_HANDLE;
        }

        for (int i = 0; i < VR_SCRIPT_N_STAGES; i++) {
                if (pipeline->modules[i] == VK_NULL_HANDLE)
                        continue;
                vkfn->vkDestroyShaderModule(window->device,
                                            pipeline->modules[i],
                                            NULL /* allocator */);
        }

        vr_free(pipeline);
}
