/*
 * vkrunner
 *
 * Copyright (C) 2018 Neil Roberts
 * Copyright (C) 2018 Intel Coporation
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

#include "vr-test.h"
#include "vr-list.h"
#include "vr-error-message.h"
#include "vr-allocate-store.h"
#include "vr-flush-memory.h"
#include "vr-box.h"
#include "vr-buffer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

enum test_resource_type {
        VR_TEST_RESOURCE_TYPE_BUFFER,
        VR_TEST_RESOURCE_TYPE_SAMPLER,
        VR_TEST_RESOURCE_TYPE_IMAGE,
};

struct test_resource {
        struct vr_list link;
        enum test_resource_type type;
        union {
                VkBuffer buffer;
                VkSampler sampler;
                VkImage image;
        };
        VkDeviceMemory memory;
        void *memory_map;
        int memory_type_index;
        union {
                /* For buffer type */
                size_t size;
        } data;
        union {
                VkBufferView buffer_view;
                VkImageView image_view;
        } coupled;
};

enum test_state {
        /* Any rendering or computing has finished and we can read the
         * buffers. */
        TEST_STATE_IDLE,
        /* The command buffer has begun */
        TEST_STATE_COMMAND_BUFFER,
        /* The render pass has begun */
        TEST_STATE_RENDER_PASS
};

struct test_data {
        struct vr_window *window;
        struct vr_pipeline *pipeline;
        struct vr_list all_resources;
        struct test_resource **resources;
        const struct vr_script *script;
        struct test_resource *vbo_buffer;
        struct test_resource *index_buffer;
        bool descriptor_set_bound;
        VkDescriptorSet *descriptor_set;
        VkPipeline bound_pipeline;
        enum test_state test_state;
        bool first_render;
};

static const double
tolerance[4] = { 0.01, 0.01, 0.01, 0.01 };

static VkDescriptorType
get_descriptor_type(enum vr_script_resource_type type)
{
        return type - VR_SCRIPT_RESOURCE_TYPE_SAMPLER +
               VK_DESCRIPTOR_TYPE_SAMPLER;
}

static VkBufferUsageFlagBits
get_buffer_usage(enum vr_script_resource_type type)
{
        uint32_t shift = type - VR_SCRIPT_RESOURCE_TYPE_UNIFORM_TEXEL;
        return VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT << shift;
}

static struct test_resource *
allocate_test_buffer(struct test_data *data,
                     size_t size,
                     VkBufferUsageFlagBits usage,
                     bool need_buffer_view)
{
        struct vr_vk *vkfn = &data->window->vkfn;
        struct test_resource *buffer = vr_calloc(sizeof *buffer);
        VkResult res;

        vr_list_insert(data->all_resources.prev, &buffer->link);

        buffer->data.size = size;
        buffer->type = VR_TEST_RESOURCE_TYPE_BUFFER;

        VkBufferCreateInfo buffer_create_info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = size,
                .usage = usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        res = vkfn->vkCreateBuffer(data->window->device,
                                   &buffer_create_info,
                                   NULL, /* allocator */
                                   &buffer->buffer);
        if (res != VK_SUCCESS) {
                buffer->buffer = VK_NULL_HANDLE;
                vr_error_message(data->window->config, "Error creating buffer");
                return NULL;
        }

        res = vr_allocate_store_buffer(data->window->context,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                       1, /* n_buffers */
                                       &buffer->buffer,
                                       &buffer->memory,
                                       &buffer->memory_type_index,
                                       NULL /* offsets */);
        if (res != VK_SUCCESS) {
                buffer->memory = VK_NULL_HANDLE;
                vr_error_message(data->window->config,
                                 "Error allocating memory");
                return NULL;
        }

        res = vkfn->vkMapMemory(data->window->device,
                                buffer->memory,
                                0, /* offset */
                                VK_WHOLE_SIZE,
                                0, /* flags */
                                &buffer->memory_map);
        if (res != VK_SUCCESS) {
                buffer->memory_map = NULL;
                vr_error_message(data->window->config, "Error mapping memory");
                return NULL;
        }

        if (!need_buffer_view)
                return buffer;

        // TODO(jaebaek): Create VkBufferView
        return buffer;
}

static void
free_test_buffer(struct test_data *data,
                 struct test_resource *buffer)
{
        struct vr_window *window = data->window;
        struct vr_vk *vkfn = &window->vkfn;

        if (buffer->memory_map) {
                vkfn->vkUnmapMemory(window->device,
                                    buffer->memory);
        }
        if (buffer->memory) {
                vkfn->vkFreeMemory(window->device,
                                   buffer->memory,
                                   NULL /* allocator */);
        }
        if (buffer->buffer) {
                vkfn->vkDestroyBuffer(window->device,
                                      buffer->buffer,
                                      NULL /* allocator */);
        }

        vr_list_remove(&buffer->link);
        vr_free(buffer);
}

static struct test_resource *
allocate_test_image(struct test_data *data,
                    VkFormat format,
                    size_t width,
                    size_t height,
                    VkImageUsageFlagBits usage)
{
        // TODO(jaebaek): Support more image formats.
        if (format != VK_FORMAT_R8G8B8A8_UNORM)
                vr_fatal("Currently only VK_FORMAT_R8G8B8A8_UNORM format images"
                         " are supported");

        struct vr_vk *vkfn = &data->window->vkfn;
        struct test_resource *image = vr_calloc(sizeof *image);
        VkResult res;

        vr_list_insert(data->all_resources.prev, &image->link);

        image->type = VR_TEST_RESOURCE_TYPE_IMAGE;

        VkImageCreateInfo image_create_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = format,
                .extent = {
                        .width = width,
                        .height = height,
                        .depth = 1
                },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_LINEAR,
                .usage = usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED
        };
        res = vkfn->vkCreateImage(data->window->device,
                                  &image_create_info,
                                  NULL, /* allocator */
                                  &image->image);
        if (res != VK_SUCCESS) {
                image->image = VK_NULL_HANDLE;
                vr_error_message(data->window->config, "Error creating image");
                return NULL;
        }

        res = vr_allocate_store_image(data->window->context,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                      1, /* n_images */
                                      &image->image,
                                      &image->memory,
                                      &image->memory_type_index);
        if (res != VK_SUCCESS) {
                image->memory = VK_NULL_HANDLE;
                vr_error_message(data->window->config,
                                 "Error allocating memory");
                return NULL;
        }

        res = vkfn->vkMapMemory(data->window->device,
                                image->memory,
                                0, /* offset */
                                VK_WHOLE_SIZE,
                                0, /* flags */
                                &image->memory_map);
        if (res != VK_SUCCESS) {
                image->memory_map = NULL;
                vr_error_message(data->window->config, "Error mapping memory");
                return NULL;
        }

        VkImageViewCreateInfo image_view_create_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image->image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = format,
                .components = {
                        .r = VK_COMPONENT_SWIZZLE_R,
                        .g = VK_COMPONENT_SWIZZLE_G,
                        .b = VK_COMPONENT_SWIZZLE_B,
                        .a = VK_COMPONENT_SWIZZLE_A
                },
                .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1
                }
        };

        res = vkfn->vkCreateImageView(data->window->device,
                                      &image_view_create_info,
                                      NULL, /* allocator */
                                      &image->coupled.image_view);
        if (res != VK_SUCCESS) {
                image->coupled.image_view = VK_NULL_HANDLE;
                vr_error_message(data->window->config,
                                 "Error creating image view");
                return false;
        }

        return image;
}

static void
free_test_image(struct test_data *data,
                struct test_resource *image)
{
        struct vr_window *window = data->window;
        struct vr_vk *vkfn = &window->vkfn;

        if (image->coupled.image_view) {
                vkfn->vkDestroyImageView(window->device,
                                         image->coupled.image_view,
                                         NULL);
        }
        if (image->memory_map) {
                vkfn->vkUnmapMemory(window->device,
                                    image->memory);
        }
        if (image->memory) {
                vkfn->vkFreeMemory(window->device,
                                   image->memory,
                                   NULL /* allocator */);
        }
        if (image->image) {
                vkfn->vkDestroyImage(window->device,
                                     image->image,
                                     NULL /* allocator */);
        }

        vr_list_remove(&image->link);
        vr_free(image);
}

static struct test_resource *
allocate_test_sampler(struct test_data *data,
                      const VkSamplerCreateInfo *sampler_create_info)
{
        struct vr_vk *vkfn = &data->window->vkfn;
        struct test_resource *sampler = vr_calloc(sizeof *sampler);
        VkResult res;

        vr_list_insert(data->all_resources.prev, &sampler->link);

        sampler->type = VR_TEST_RESOURCE_TYPE_SAMPLER;

        res = vkfn->vkCreateSampler(data->window->device,
                                    sampler_create_info,
                                    NULL, /* allocator */
                                    &sampler->sampler);
        if (res != VK_SUCCESS) {
                sampler->sampler = VK_NULL_HANDLE;
                vr_error_message(data->window->config,
                                 "Error creating sampler");
                return NULL;
        }
        return sampler;
}

static void
free_test_sampler(struct test_data *data,
                  struct test_resource *sampler)
{
        struct vr_window *window = data->window;
        struct vr_vk *vkfn = &window->vkfn;
        if (sampler->sampler) {
                vkfn->vkDestroySampler(window->device,
                                       sampler->sampler,
                                       NULL /* allocator */);
        }
        vr_list_remove(&sampler->link);
        vr_free(sampler);
}

static bool
begin_command_buffer(struct test_data *data)
{
        struct vr_vk *vkfn = &data->window->vkfn;
        VkResult res;

        VkCommandBufferBeginInfo begin_command_buffer_info = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
        };
        res = vkfn->vkBeginCommandBuffer(data->window->context->command_buffer,
                                         &begin_command_buffer_info);
        if (res != VK_SUCCESS) {
                vr_error_message(data->window->config,
                                 "vkBeginCommandBuffer failed");
                return false;
        }

        return true;
}

static void
invalidate_ssbos(struct test_data *data)
{
        struct vr_vk *vkfn = &data->window->vkfn;

        for (unsigned i = 0; i < data->script->n_resources; i++) {
                if (data->script->resources[i].type !=
                    VR_SCRIPT_RESOURCE_TYPE_STORAGE_BUFFER)
                        continue;

                const struct test_resource *buffer = data->resources[i];

                const VkMemoryType *memory_type =
                        (data->window->context->memory_properties.memoryTypes +
                         buffer->memory_type_index);

                /* We don’t need to do anything if the memory is
                 * already coherent */
                if ((memory_type->propertyFlags &
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                        continue;

                VkMappedMemoryRange memory_range = {
                        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                        .memory = data->resources[i]->memory,
                        .offset = 0,
                        .size = VK_WHOLE_SIZE
                };
                vkfn->vkInvalidateMappedMemoryRanges(data->window->device,
                                                     1, /* memoryRangeCount */
                                                     &memory_range);
        }
}

static bool
end_command_buffer(struct test_data *data)
{
        VkResult res;
        struct vr_window *window = data->window;
        struct vr_context *context = window->context;
        struct vr_vk *vkfn = &context->vkfn;

        res = vkfn->vkEndCommandBuffer(context->command_buffer);
        if (res != VK_SUCCESS) {
                vr_error_message(context->config,
                                 "vkEndCommandBuffer failed");
                return false;
        }

        vkfn->vkResetFences(context->device,
                            1, /* fenceCount */
                            &context->vk_fence);

        VkSubmitInfo submit_info = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &context->command_buffer,
                .pWaitDstStageMask =
                (VkPipelineStageFlags[])
                { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT }
        };
        res = vkfn->vkQueueSubmit(context->queue,
                                  1, /* submitCount */
                                  &submit_info,
                                  context->vk_fence);
        if (res != VK_SUCCESS) {
                vr_error_message(context->config, "vkQueueSubmit failed");
                return false;
        }

        res = vkfn->vkWaitForFences(context->device,
                                    1, /* fenceCount */
                                    &context->vk_fence,
                                    VK_TRUE, /* waitAll */
                                    UINT64_MAX);
        if (res != VK_SUCCESS) {
                vr_error_message(context->config, "vkWaitForFences failed");
                return false;
        }

        if (window->need_linear_memory_invalidate) {
                VkMappedMemoryRange memory_range = {
                        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                        .memory = window->linear_memory,
                        .offset = 0,
                        .size = VK_WHOLE_SIZE
                };
                vkfn->vkInvalidateMappedMemoryRanges(window->device,
                                                     1, /* memoryRangeCount */
                                                     &memory_range);
        }

        invalidate_ssbos(data);

        return true;
}

static bool
begin_render_pass(struct test_data *data)
{
        struct vr_vk *vkfn = &data->window->vkfn;

        VkRenderPassBeginInfo render_pass_begin_info = {
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass = (data->first_render ?
                               data->window->render_pass[0] :
                               data->window->render_pass[1]),
                .framebuffer = data->window->framebuffer,
                .renderArea = {
                        .offset = { 0, 0 },
                        .extent = {
                                VR_WINDOW_WIDTH, VR_WINDOW_HEIGHT
                        }
                },
        };
        vkfn->vkCmdBeginRenderPass(data->window->context->command_buffer,
                                   &render_pass_begin_info,
                                   VK_SUBPASS_CONTENTS_INLINE);

        data->bound_pipeline = VK_NULL_HANDLE;
        data->descriptor_set_bound = false;
        data->first_render = false;

        return true;
}

static bool
end_render_pass(struct test_data *data)
{
        struct vr_window *window = data->window;
        struct vr_vk *vkfn = &window->vkfn;

        vkfn->vkCmdEndRenderPass(window->context->command_buffer);

        VkBufferImageCopy copy_region = {
                .bufferOffset = 0,
                .bufferRowLength = VR_WINDOW_WIDTH,
                .bufferImageHeight = VR_WINDOW_HEIGHT,
                .imageSubresource = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1
                },
                .imageOffset = { 0, 0, 0 },
                .imageExtent = { VR_WINDOW_WIDTH, VR_WINDOW_HEIGHT, 1 }
        };
        vkfn->vkCmdCopyImageToBuffer(window->context->command_buffer,
                                     window->color_image,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     window->linear_buffer,
                                     1, /* regionCount */
                                     &copy_region);

        return true;
}

static bool
set_state(struct test_data *data,
          enum test_state state)
{
        while (data->test_state < state) {
                switch (data->test_state) {
                case TEST_STATE_IDLE:
                        if (!begin_command_buffer(data))
                                return false;
                        break;
                case TEST_STATE_COMMAND_BUFFER:
                        if (!begin_render_pass(data))
                                return false;
                        break;
                case TEST_STATE_RENDER_PASS:
                        vr_fatal("Unexpected test state");
                }
                data->test_state++;
        }

        while (data->test_state > state) {
                switch (data->test_state) {
                case TEST_STATE_IDLE:
                        vr_fatal("Unexpected test state");
                case TEST_STATE_COMMAND_BUFFER:
                        if (!end_command_buffer(data))
                                return false;
                        break;
                case TEST_STATE_RENDER_PASS:
                        if (!end_render_pass(data))
                                return false;
                        break;
                }
                data->test_state--;
        }

        return true;
}

static void
bind_ubo_descriptor_set(struct test_data *data)
{
        struct vr_vk *vkfn = &data->window->vkfn;
        const struct vr_context *context = data->window->context;

        if (data->descriptor_set_bound || !data->descriptor_set)
                return;

        if (data->pipeline->stages & ~VK_SHADER_STAGE_COMPUTE_BIT) {
                for (unsigned i = 0; i < data->pipeline->n_desc_sets; i++) {
                        if (data->descriptor_set[i] == VK_NULL_HANDLE) {
                                vr_error_message(data->window->config,
                                                 "Warning buffer descriptor set"
                                                 " [%u] is VK_NULL_HANDLE",
                                                 i);
                                continue;
                        }
                        vkfn->vkCmdBindDescriptorSets(
                                        context->command_buffer,
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        data->pipeline->layout,
                                        /* firstSet */
                                        data->pipeline->desc_sets[i],
                                        1, /* descriptorSetCount */
                                        &data->descriptor_set[i],
                                        0, /* dynamicOffsetCount */
                                        NULL /* pDynamicOffsets */);
                }
        }

        if (data->pipeline->compute_pipeline) {
                for (unsigned i = 0; i < data->pipeline->n_desc_sets; i++) {
                        if (data->descriptor_set[i] == VK_NULL_HANDLE)
                                continue;
                        vkfn->vkCmdBindDescriptorSets(
                                        context->command_buffer,
                                        VK_PIPELINE_BIND_POINT_COMPUTE,
                                        data->pipeline->layout,
                                        /* firstSet */
                                        data->pipeline->desc_sets[i],
                                        1, /* descriptorSetCount */
                                        &data->descriptor_set[i],
                                        0, /* dynamicOffsetCount */
                                        NULL /* pDynamicOffsets */);
                }
        }

        data->descriptor_set_bound = true;
}

static void
bind_pipeline(struct test_data *data,
              VkPipeline pipeline)
{
        struct vr_vk *vkfn = &data->window->vkfn;
        struct vr_context *context = data->window->context;

        if (pipeline == data->bound_pipeline)
                return;

        if (pipeline == data->pipeline->compute_pipeline) {
                vkfn->vkCmdBindPipeline(context->command_buffer,
                                        VK_PIPELINE_BIND_POINT_COMPUTE,
                                        pipeline);
        } else {
                vkfn->vkCmdBindPipeline(context->command_buffer,
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline);
        }

        data->bound_pipeline = pipeline;
}

static void
print_command_fail(const struct vr_config *config,
                   const struct vr_script_command *command)
{
        vr_error_message(config,
                         "Command failed at line %i",
                         command->line_num);
}

static struct test_resource *
get_resource(struct test_data *data,
             int desc_set,
             int binding)
{
        for (unsigned i = 0; i < data->script->n_resources; i++) {
                if (data->script->resources[i].binding == binding &&
                    data->script->resources[i].desc_set == desc_set)
                        return data->resources[i];
        }

        return NULL;
}

static bool
draw_rect(struct test_data *data,
          const struct vr_script_command *command)
{
        struct vr_vk *vkfn = &data->window->vkfn;
        struct test_resource *buffer;

        buffer = allocate_test_buffer(data,
                                      sizeof (struct vr_pipeline_vertex) * 4,
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                      false);
        if (buffer == NULL)
                return false;

        if (!set_state(data, TEST_STATE_RENDER_PASS))
                return false;

        struct vr_pipeline_vertex *v = buffer->memory_map;

        v->x = command->draw_rect.x;
        v->y = command->draw_rect.y;
        v->z = 0.0f;
        v++;

        v->x = command->draw_rect.x + command->draw_rect.w;
        v->y = command->draw_rect.y;
        v->z = 0.0f;
        v++;

        v->x = command->draw_rect.x;
        v->y = command->draw_rect.y + command->draw_rect.h;
        v->z = 0.0f;
        v++;

        v->x = command->draw_rect.x + command->draw_rect.w;
        v->y = command->draw_rect.y + command->draw_rect.h;
        v->z = 0.0f;
        v++;

        vr_flush_memory(data->window->context,
                        buffer->memory_type_index,
                        buffer->memory,
                        0, /* offset */
                        VK_WHOLE_SIZE);

        bind_ubo_descriptor_set(data);
        VkPipeline pipeline =
                data->pipeline->pipelines[command->draw_rect.pipeline_key];
        bind_pipeline(data, pipeline);

        vkfn->vkCmdBindVertexBuffers(data->window->context->command_buffer,
                                     0, /* firstBinding */
                                     1, /* bindingCount */
                                     &buffer->buffer,
                                     (VkDeviceSize[]) { 0 });
        vkfn->vkCmdDraw(data->window->context->command_buffer,
                        4, /* vertexCount */
                        1, /* instanceCount */
                        0, /* firstVertex */
                        0 /* firstInstance */);

        return true;
}

static bool
ensure_index_buffer(struct test_data *data)
{
        if (data->index_buffer)
                return true;

        data->index_buffer =
                allocate_test_buffer(data,
                                     data->script->n_indices *
                                     sizeof data->script->indices[0],
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                     false);
        if (data->index_buffer == NULL)
                return false;

        memcpy(data->index_buffer->memory_map,
               data->script->indices,
               data->script->n_indices * sizeof data->script->indices[0]);

        vr_flush_memory(data->window->context,
                        data->index_buffer->memory_type_index,
                        data->index_buffer->memory,
                        0, /* offset */
                        VK_WHOLE_SIZE);

        return true;
}

static bool
ensure_vbo_buffer(struct test_data *data)
{
        struct vr_vbo *vbo = data->script->vertex_data;

        if (vbo == NULL || data->vbo_buffer)
                return true;

        data->vbo_buffer =
                allocate_test_buffer(data,
                                     vbo->stride *
                                     vbo->num_rows,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     false);
        if (data->vbo_buffer == NULL)
                return false;

        memcpy(data->vbo_buffer->memory_map,
               vbo->raw_data,
               vbo->stride * vbo->num_rows);

        vr_flush_memory(data->window->context,
                        data->vbo_buffer->memory_type_index,
                        data->vbo_buffer->memory,
                        0, /* offset */
                        VK_WHOLE_SIZE);

        return true;
}

static bool
draw_arrays(struct test_data *data,
            const struct vr_script_command *command)
{
        struct vr_vk *vkfn = &data->window->vkfn;
        struct vr_context *context = data->window->context;

        if (!set_state(data, TEST_STATE_RENDER_PASS))
                return false;

        struct vr_vbo *vbo = data->script->vertex_data;

        if (vbo) {
                if (!ensure_vbo_buffer(data))
                        return false;

                vkfn->vkCmdBindVertexBuffers(context->command_buffer,
                                             0, /* firstBinding */
                                             1, /* bindingCount */
                                             &data->vbo_buffer->buffer,
                                             (VkDeviceSize[]) { 0 });

        }

        bind_ubo_descriptor_set(data);
        VkPipeline pipeline =
                data->pipeline->pipelines[command->draw_arrays.pipeline_key];
        bind_pipeline(data, pipeline);

        if (command->draw_arrays.indexed) {
                if (!ensure_index_buffer(data))
                        return false;
                vkfn->vkCmdBindIndexBuffer(context->command_buffer,
                                           data->index_buffer->buffer,
                                           0, /* offset */
                                           VK_INDEX_TYPE_UINT16);
                vkfn->vkCmdDrawIndexed(context->command_buffer,
                                       command->draw_arrays.vertex_count,
                                       command->draw_arrays.instance_count,
                                       0, /* firstIndex */
                                       command->draw_arrays.first_vertex,
                                       command->draw_arrays.first_instance);
        } else {
                vkfn->vkCmdDraw(context->command_buffer,
                                command->draw_arrays.vertex_count,
                                command->draw_arrays.instance_count,
                                command->draw_arrays.first_vertex,
                                command->draw_arrays.first_instance);
        }

        return true;
}

static bool
dispatch_compute(struct test_data *data,
                 const struct vr_script_command *command)
{
        struct vr_vk *vkfn = &data->window->vkfn;

        if (!set_state(data, TEST_STATE_COMMAND_BUFFER))
                return false;

        bind_ubo_descriptor_set(data);
        bind_pipeline(data, data->pipeline->compute_pipeline);

        vkfn->vkCmdDispatch(data->window->context->command_buffer,
                            command->dispatch_compute.x,
                            command->dispatch_compute.y,
                            command->dispatch_compute.z);

        return true;
}

static bool
compare_pixels(const double *color1,
               const double *color2,
               const double *tolerance,
               int n_components)
{
        for (int p = 0; p < n_components; ++p)
                if (fabs(color1[p] - color2[p]) > tolerance[p])
                        return false;
        return true;
}

static void
print_components_double(struct vr_buffer *buf,
                        const double *pixel,
                        int n_components)
{
        for (int p = 0; p < n_components; ++p)
                vr_buffer_append_printf(buf, " %f", pixel[p]);
}

static void
print_bad_pixel(const struct vr_config *config,
                int x, int y,
                int n_components,
                const double *expected,
                const double *observed)
{
        struct vr_buffer buf = VR_BUFFER_STATIC_INIT;

        vr_buffer_append_printf(&buf,
                                "Probe color at (%i,%i)\n"
                                "  Expected:",
                                x, y);
        print_components_double(&buf, expected, n_components);
        vr_buffer_append_string(&buf,
                                "\n"
                                "  Observed:");
        print_components_double(&buf, observed, n_components);

        vr_error_message(config, "%s", (const char *) buf.data);

        vr_buffer_destroy(&buf);
}

static bool
probe_rect(struct test_data *data,
           const struct vr_script_command *command)
{
        int n_components = command->probe_rect.n_components;
        const struct vr_format *format = data->window->framebuffer_format;
        int format_size = vr_format_get_size(format);

        /* End the paint to copy the framebuffer into the linear buffer */
        if (!set_state(data, TEST_STATE_IDLE))
                return false;

        for (int y = 0; y < command->probe_rect.h; y++) {
                const uint8_t *p =
                        ((y + command->probe_rect.y) *
                         data->window->linear_memory_stride +
                         command->probe_rect.x * format_size +
                         (uint8_t *) data->window->linear_memory_map);
                for (int x = 0; x < command->probe_rect.w; x++) {
                        double pixel[4];
                        vr_format_load_pixel(format, p, pixel);
                        p += format_size;

                        if (!compare_pixels(pixel,
                                            command->probe_rect.color,
                                            tolerance,
                                            n_components)) {
                                print_command_fail(data->window->config,
                                                   command);
                                print_bad_pixel(data->window->config,
                                                x + command->probe_rect.x,
                                                y + command->probe_rect.y,
                                                n_components,
                                                command->probe_rect.color,
                                                pixel);
                                return false;
                        }
                }
        }

        return true;
}

struct append_box_closure {
        struct vr_buffer *buf;
        const struct vr_box *value;
};

static bool
append_box_cb(enum vr_box_base_type base_type,
              size_t offset,
              void *user_data)
{
        struct append_box_closure *data = user_data;
        const uint8_t *p = (const uint8_t *) data->value + offset;

        vr_buffer_append_c(data->buf, ' ');

        switch (base_type) {
        case VR_BOX_BASE_TYPE_INT:
                vr_buffer_append_printf(data->buf, "%i", *(const int *) p);
                break;
        case VR_BOX_BASE_TYPE_UINT:
                vr_buffer_append_printf(data->buf, "%u", *(const unsigned *) p);
                break;
        case VR_BOX_BASE_TYPE_INT8:
                vr_buffer_append_printf(data->buf,
                                        "%" PRIi8,
                                        *(const int8_t *) p);
                break;
        case VR_BOX_BASE_TYPE_UINT8:
                vr_buffer_append_printf(data->buf,
                                        "%" PRIu8,
                                        *(const uint8_t *) p);
                break;
        case VR_BOX_BASE_TYPE_INT16:
                vr_buffer_append_printf(data->buf,
                                        "%" PRIi16,
                                        *(const int16_t *) p);
                break;
        case VR_BOX_BASE_TYPE_UINT16:
                vr_buffer_append_printf(data->buf,
                                        "%" PRIu16,
                                        *(const uint16_t *) p);
                break;
        case VR_BOX_BASE_TYPE_INT64:
                vr_buffer_append_printf(data->buf,
                                        "%" PRIi64,
                                        *(const int64_t *) p);
                break;
        case VR_BOX_BASE_TYPE_UINT64:
                vr_buffer_append_printf(data->buf,
                                        "%" PRIu64,
                                        *(const uint64_t *) p);
                break;
        case VR_BOX_BASE_TYPE_FLOAT:
                vr_buffer_append_printf(data->buf, "%f", *(const float *) p);
                break;
        case VR_BOX_BASE_TYPE_DOUBLE:
                vr_buffer_append_printf(data->buf, "%f", *(const double *) p);
                break;
        }

        return true;
}

static void
append_box(struct vr_buffer *buf,
           const struct vr_box *value)
{
        struct append_box_closure data = {
                .buf = buf,
                .value = value
        };

        vr_box_for_each_component(value->type,
                                  append_box_cb,
                                  &data);
}

static bool
probe_ssbo(struct test_data *data,
           const struct vr_script_command *command)
{
        if (!set_state(data, TEST_STATE_IDLE))
                return false;

        struct test_resource *buffer =
                get_resource(data,
                             command->probe_ssbo.desc_set,
                             command->probe_ssbo.binding);

        if (buffer == NULL) {
                print_command_fail(data->window->config, command);
                vr_error_message(data->window->config,
                                 "Invalid binding in probe command");
                return false;
        }

        const struct vr_box *expected = &command->probe_ssbo.value;
        size_t type_size = vr_box_type_size(expected->type);

        if (command->probe_ssbo.offset + type_size > buffer->data.size) {
                print_command_fail(data->window->config, command);
                vr_error_message(data->window->config,
                                 "Invalid offset in probe command");
                return false;
        }

        struct vr_box observed;
        observed.type = expected->type;
        memcpy(&observed.i,
               (const uint8_t *) buffer->memory_map +
               command->probe_ssbo.offset,
               type_size);

        if (!vr_box_compare(command->probe_ssbo.comparison,
                            &observed,
                            expected)) {
                print_command_fail(data->window->config, command);

                struct vr_buffer buf = VR_BUFFER_STATIC_INIT;
                vr_buffer_append_string(&buf,
                                        "SSBO probe failed\n"
                                        "  Reference:");
                append_box(&buf, expected);
                vr_buffer_append_string(&buf,
                                        "\n"
                                        "  Observed: ");
                append_box(&buf, &observed);
                vr_error_message(data->window->config,
                                 "%s",
                                 (const char *) buf.data);
                vr_buffer_destroy(&buf);

                return false;
        }

        return true;
}

static bool
set_push_constant(struct test_data *data,
                  const struct vr_script_command *command)
{
        struct vr_vk *vkfn = &data->window->vkfn;

        if (data->test_state < TEST_STATE_COMMAND_BUFFER &&
            !set_state(data, TEST_STATE_COMMAND_BUFFER))
                return false;

        vkfn->vkCmdPushConstants(data->window->context->command_buffer,
                                 data->pipeline->layout,
                                 data->pipeline->stages,
                                 command->set_push_constant.offset,
                                 command->set_push_constant.size,
                                 command->set_push_constant.data);

        return true;
}

static bool
allocate_resources(struct test_data *data)
{
        struct vr_vk *vkfn = &data->window->vkfn;
        unsigned desc_set;
        unsigned desc_set_index;

        VkResult res;
        data->descriptor_set = vr_alloc(data->pipeline->n_desc_sets *
                                            sizeof(VkDescriptorSet *));
        for (unsigned i = 0; i < data->pipeline->n_desc_sets; i++) {
                VkDescriptorSetAllocateInfo allocate_info = {
                        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                        .descriptorPool =
                                data->window->context->descriptor_pool,
                        .descriptorSetCount = 1,
                        .pSetLayouts = &data->pipeline->descriptor_set_layout[i]
                };
                res = vkfn->vkAllocateDescriptorSets(
                                data->window->device,
                                &allocate_info,
                                &data->descriptor_set[i]);
                if (res != VK_SUCCESS) {
                        data->descriptor_set[i] = VK_NULL_HANDLE;
                        vr_error_message(data->window->config,
                                         "Error allocating descriptor set");
                        return false;
                }
        }

        data->resources = vr_alloc(sizeof *data->resources *
                                     data->script->n_resources);

        desc_set = data->script->resources[0].desc_set;
        desc_set_index = 0;
        for (unsigned i = 0; i < data->script->n_resources; i++) {
                const struct vr_script_resource *script_resource =
                        data->script->resources + i;

                if (script_resource->desc_set != desc_set) {
                        desc_set = script_resource->desc_set;
                        ++desc_set_index;
                }

                VkWriteDescriptorSet write = {
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = data->descriptor_set[desc_set_index],
                        .dstBinding = script_resource->binding,
                        .dstArrayElement = 0,
                        .descriptorCount = 1,
                        .descriptorType =
                                get_descriptor_type(script_resource->type),
                };

                VkDescriptorBufferInfo buffer_info = {
                        .offset = 0,
                        .range = VK_WHOLE_SIZE
                };
                VkDescriptorImageInfo image_info = {0};
                switch (script_resource->type) {
                // Sampler.
                case VR_SCRIPT_RESOURCE_TYPE_SAMPLER:
                        ((void) 0);

                        struct test_resource *sampler =
                                allocate_test_sampler(
                                        data,
                                        &script_resource->image.sampler);
                        if (sampler == NULL)
                                return false;
                        image_info.sampler = sampler->sampler;
                        write.pImageInfo = &image_info;
                        break;

                // Images.
                case VR_SCRIPT_RESOURCE_TYPE_COMBINED_IMAGE:
                case VR_SCRIPT_RESOURCE_TYPE_SAMPLED_IMAGE:
                case VR_SCRIPT_RESOURCE_TYPE_STORAGE_IMAGE:
                        ((void) 0);

                        VkImageUsageFlagBits image_usage;
                        if (script_resource->type ==
                            VR_SCRIPT_RESOURCE_TYPE_STORAGE_IMAGE) {
                                image_usage =
                                        VK_IMAGE_USAGE_STORAGE_BIT;
                        } else {
                                image_usage =
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
                        }

                        VkFormat format =
                                script_resource->image.format->vk_format;
                        struct test_resource *image =
                                allocate_test_image(
                                        data,
                                        format,
                                        script_resource->image.width,
                                        script_resource->image.height,
                                        image_usage);

                        if (image == NULL)
                                return false;

                        data->resources[i] = image;

                        if (script_resource->type ==
                            VR_SCRIPT_RESOURCE_TYPE_COMBINED_IMAGE) {
                                sampler =
                                        allocate_test_sampler(
                                                data,
                                                &script_resource->
                                                image.sampler);
                                if (sampler == NULL)
                                        return false;
                                image_info.sampler = sampler->sampler;
                        }
                        image_info.imageView = image->coupled.image_view;
                        image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                        write.pImageInfo = &image_info;
                        break;

                // Buffers.
                case VR_SCRIPT_RESOURCE_TYPE_UNIFORM_TEXEL:
                case VR_SCRIPT_RESOURCE_TYPE_STORAGE_TEXEL:
                case VR_SCRIPT_RESOURCE_TYPE_UNIFORM_BUFFER:
                case VR_SCRIPT_RESOURCE_TYPE_STORAGE_BUFFER:
                        ((void) 0);

                        bool need_buffer_view =
                                script_resource->type ==
                                VR_SCRIPT_RESOURCE_TYPE_UNIFORM_TEXEL ||
                                script_resource->type ==
                                VR_SCRIPT_RESOURCE_TYPE_STORAGE_TEXEL;

                        struct test_resource *buffer =
                                allocate_test_buffer(
                                        data,
                                        script_resource->buffer.size,
                                        get_buffer_usage(script_resource->type),
                                        need_buffer_view);

                        if (buffer == NULL)
                                return false;

                        data->resources[i] = buffer;

                        buffer_info.buffer = buffer->buffer;
                        write.pBufferInfo = &buffer_info;
                        break;
                default:
                        vr_fatal("Unexpected buffer type");
                }

                vkfn->vkUpdateDescriptorSets(
                        data->window->device,
                        1, /* descriptorWriteCount */
                        &write,
                        0, /* descriptorCopyCount */
                        NULL /* pDescriptorCopies */);
        }

        return true;
}

static bool
dump_resources(struct test_data *data)
{
        struct vr_buffer buf = VR_BUFFER_STATIC_INIT;
        const char *resource_name = NULL;
        const char *p;

        vr_buffer_append_printf(&buf,
                                "Dump %lu resources ...\n\n",
                                data->script->n_resources);

        for (unsigned i = 0; i < data->script->n_resources; i++) {
                const struct vr_script_resource *script_resource =
                        data->script->resources + i;

                switch (script_resource->type) {
                // Images.
                case VR_SCRIPT_RESOURCE_TYPE_COMBINED_IMAGE:
                        resource_name = "combined image";
                case VR_SCRIPT_RESOURCE_TYPE_SAMPLED_IMAGE:
                        resource_name = "sampled image";
                case VR_SCRIPT_RESOURCE_TYPE_STORAGE_IMAGE:
                        resource_name = "storage image";

                        p = data->resources[i]->memory_map;
                        size_t width = script_resource->image.width;
                        size_t height = script_resource->image.height;
                        vr_buffer_append_printf(&buf,
                                                "%u. %u:%u %s\n",
                                                i,
                                                script_resource->desc_set,
                                                script_resource->binding,
                                                resource_name);
                        for (size_t y = 0; y < height; ++y) {
                                for (size_t x = 0; x < width; ++x) {
                                        for (size_t j = 0; j < 4; ++j) {
                                                vr_buffer_append_printf(
                                                        &buf,
                                                        "%02x",
                                                        p[j] & 0xff);
                                        }
                                        p += 4;
                                }
                                vr_buffer_append_printf(&buf, "\n");
                        }
                        break;

                // Buffers.
                case VR_SCRIPT_RESOURCE_TYPE_UNIFORM_TEXEL:
                        resource_name = "uniform texel";
                case VR_SCRIPT_RESOURCE_TYPE_STORAGE_TEXEL:
                        resource_name = "storage texel";
                case VR_SCRIPT_RESOURCE_TYPE_UNIFORM_BUFFER:
                        resource_name = "uniform buffer";
                case VR_SCRIPT_RESOURCE_TYPE_STORAGE_BUFFER:
                        resource_name = "storage buffer";

                        p = data->resources[i]->memory_map;
                        size_t size = script_resource->buffer.size;
                        vr_buffer_append_printf(&buf,
                                                "%u. %u:%u %s %lu\n",
                                                i,
                                                script_resource->desc_set,
                                                script_resource->binding,
                                                resource_name,
                                                size);
                        for (size_t j = 0; j < size; ++j)
                                vr_buffer_append_printf(&buf,
                                                        "%02x",
                                                        p[j] & 0xff);
                        vr_buffer_append_printf(&buf, "\n");
                        break;
                default:
                        vr_buffer_append_printf(&buf,
                                                "%u. %u:%u not buffer nor "
                                                "image ... skip\n\n",
                                                i,
                                                script_resource->desc_set,
                                                script_resource->binding);
                        break;
                }
                vr_buffer_append_printf(&buf, "\n");
        }

        vr_error_message(data->window->config, "%s", (const char *) buf.data);

        vr_buffer_destroy(&buf);

        return true;
}

static bool
set_buffer_subdata(struct test_data *data,
                   const struct vr_script_command *command)
{
        struct test_resource *buffer =
                get_resource(data,
                             command->set_buffer_subdata.desc_set,
                             command->set_buffer_subdata.binding);
        assert(buffer);

        memcpy((uint8_t *) buffer->memory_map +
               command->set_buffer_subdata.offset,
               command->set_buffer_subdata.data,
               command->set_buffer_subdata.size);
        vr_flush_memory(data->window->context,
                        buffer->memory_type_index,
                        buffer->memory,
                        command->set_push_constant.offset,
                        command->set_buffer_subdata.size);

        return true;
}

static void
set_color(enum vr_script_image_color color,
          size_t width,
          size_t height,
          uint32_t *memory)
{
        // TODO(jaebaek): Currently, we assume that the format is
        // R8G8B8A8_UNORM. Support other formats.
        for (size_t y = 0; y < height; ++y) {
                for (size_t x = 0; x < width; ++x) {
                        uint32_t rgb;
                        if (color == VR_SCRIPT_IMAGE_COLOR_RGBW) {
                                if (x < width / 2 && y < height / 2)
                                        rgb = 0xff0000ff;
                                else if (x >= width / 2 && y < height / 2)
                                        rgb = 0xff00ff00;
                                else if (x < width / 2 && y >= height / 2)
                                        rgb = 0xffff0000;
                                else
                                        rgb = 0xffffffff;
                        } else if (color == VR_SCRIPT_IMAGE_COLOR_RED) {
                                        rgb = 0xff0000ff;
                        } else if (color == VR_SCRIPT_IMAGE_COLOR_GREEN) {
                                        rgb = 0xff00ff00;
                        } else if (color == VR_SCRIPT_IMAGE_COLOR_BLUE) {
                                        rgb = 0xffff0000;
                        }
                        memory[width * y + x] = rgb;
                }
        }
}

static bool
set_image_color(struct test_data *data,
               const struct vr_script_command *command)
{
        struct test_resource *image =
                get_resource(data,
                             command->set_image_color.desc_set,
                             command->set_image_color.binding);
        assert(image);

        set_color(command->set_image_color.color,
                  command->set_image_color.width,
                  command->set_image_color.height,
                  image->memory_map);

        VkDeviceSize size =
                command->set_image_color.width *
                command->set_image_color.height *
                sizeof(uint32_t);
        vr_flush_memory(data->window->context,
                        image->memory_type_index,
                        image->memory,
                        0,
                        size);

        return true;
}

static bool
clear(struct test_data *data,
      const struct vr_script_command *command)
{
        struct vr_vk *vkfn = &data->window->vkfn;

        if (!set_state(data, TEST_STATE_RENDER_PASS))
                return false;

        VkImageAspectFlags depth_stencil_flags = 0;
        const struct vr_format *depth_stencil_format =
                data->window->depth_stencil_format;

        if (data->window->depth_stencil_format) {
                for (int i = 0; i < depth_stencil_format->n_parts; i++) {
                        switch (depth_stencil_format->parts[i].component) {
                        case VR_FORMAT_COMPONENT_D:
                                depth_stencil_flags |=
                                        VK_IMAGE_ASPECT_DEPTH_BIT;
                                break;
                        case VR_FORMAT_COMPONENT_S:
                                depth_stencil_flags |=
                                        VK_IMAGE_ASPECT_STENCIL_BIT;
                                break;
                        default:
                                break;
                        }
                }
        }

        VkClearAttachment clear_attachments[] = {
                {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .colorAttachment = 0,
                },
                {
                        .aspectMask = depth_stencil_flags,
                        .clearValue = { .depthStencil =
                                        { .depth = command->clear.depth,
                                          .stencil = command->clear.stencil } }
                },
        };
        VkClearRect clear_rect = {
                .rect = {
                        .offset = { 0, 0 },
                        .extent = { VR_WINDOW_WIDTH, VR_WINDOW_HEIGHT }
                },
                .baseArrayLayer = 0,
                .layerCount = 1
        };
        memcpy(clear_attachments[0].clearValue.color.float32,
               command->clear.color,
               sizeof command->clear.color);

        int n_attachments;

        if (depth_stencil_flags)
                n_attachments = 2;
        else
                n_attachments = 1;

        vkfn->vkCmdClearAttachments(data->window->context->command_buffer,
                                    n_attachments,
                                    clear_attachments,
                                    1,
                                    &clear_rect);
        return true;
}

static bool
run_commands(struct test_data *data)
{
        const struct vr_script *script = data->script;
        bool ret = true;

        for (int i = 0; i < script->n_commands; i++) {
                const struct vr_script_command *command = script->commands + i;

                switch (command->op) {
                case VR_SCRIPT_OP_DRAW_RECT:
                        if (!draw_rect(data, command))
                                ret = false;
                        break;
                case VR_SCRIPT_OP_DRAW_ARRAYS:
                        if (!draw_arrays(data, command))
                                ret = false;
                        break;
                case VR_SCRIPT_OP_DISPATCH_COMPUTE:
                        if (!dispatch_compute(data, command))
                                ret = false;
                        break;
                case VR_SCRIPT_OP_PROBE_RECT:
                        if (!probe_rect(data, command))
                                ret = false;
                        break;
                case VR_SCRIPT_OP_PROBE_SSBO:
                        if (!probe_ssbo(data, command))
                                ret = false;
                        break;
                case VR_SCRIPT_OP_SET_PUSH_CONSTANT:
                        if (!set_push_constant(data, command))
                                ret = false;
                        break;
                case VR_SCRIPT_OP_SET_BUFFER_SUBDATA:
                        if (!set_buffer_subdata(data, command))
                                ret = false;
                        break;
                case VR_SCRIPT_OP_SET_IMAGE_COLOR:
                        if (!set_image_color(data, command))
                                ret = false;
                        break;
                case VR_SCRIPT_OP_CLEAR:
                        if (!clear(data, command))
                                ret = false;
                        break;
                }
        }

        return ret;
}

bool
vr_test_run(struct vr_window *window,
            struct vr_pipeline *pipeline,
            const struct vr_script *script)
{
        struct vr_vk *vkfn = &window->vkfn;

        struct test_data data = {
                .window = window,
                .pipeline = pipeline,
                .script = script,
                .test_state = TEST_STATE_IDLE,
                .first_render = true,
        };
        bool ret = true;

        vr_list_init(&data.all_resources);

        if (script->n_resources > 0 && !allocate_resources(&data)) {
                ret = false;
        } else {
                if (!run_commands(&data))
                        ret = false;

                if (!set_state(&data, TEST_STATE_IDLE))
                        ret = false;
        }

        if (window->config->dump_resources)
                dump_resources(&data);

        struct test_resource *resource, *tmp;
        vr_list_for_each_safe(resource, tmp, &data.all_resources, link) {
                switch (resource->type) {
                case VR_TEST_RESOURCE_TYPE_BUFFER:
                        free_test_buffer(&data, resource);
                        break;
                case VR_TEST_RESOURCE_TYPE_SAMPLER:
                        free_test_sampler(&data, resource);
                        break;
                case VR_TEST_RESOURCE_TYPE_IMAGE:
                        free_test_image(&data, resource);
                        break;
                default:
                        vr_fatal("Unexpected resource type");
                }
        }

        vr_free(data.resources);

        if (data.descriptor_set) {
                for (unsigned i = 0; i < pipeline->n_desc_sets; i++) {
                        if (data.descriptor_set[i]) {
                                vkfn->vkFreeDescriptorSets(
                                                window->device,
                                                window->context->
                                                descriptor_pool,
                                                1, /* descriptorSetCount */
                                                &data.descriptor_set[i]);
                        }
                }
                vr_free(data.descriptor_set);
        }

        return ret;
}
