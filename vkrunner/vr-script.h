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

#ifndef VR_SCRIPT_H
#define VR_SCRIPT_H

#include "vr-list.h"
#include "vr-vk.h"
#include "vr-vbo.h"
#include "vr-format.h"
#include "vr-pipeline-key.h"
#include "vr-config-private.h"
#include "vr-box.h"

enum vr_script_shader_stage {
        VR_SCRIPT_SHADER_STAGE_VERTEX,
        VR_SCRIPT_SHADER_STAGE_TESS_CTRL,
        VR_SCRIPT_SHADER_STAGE_TESS_EVAL,
        VR_SCRIPT_SHADER_STAGE_GEOMETRY,
        VR_SCRIPT_SHADER_STAGE_FRAGMENT,
        VR_SCRIPT_SHADER_STAGE_COMPUTE
};

#define VR_SCRIPT_N_STAGES 6

enum vr_script_op {
        VR_SCRIPT_OP_DRAW_RECT,
        VR_SCRIPT_OP_DRAW_ARRAYS,
        VR_SCRIPT_OP_DISPATCH_COMPUTE,
        VR_SCRIPT_OP_PROBE_RECT,
        VR_SCRIPT_OP_PROBE_SSBO,
        VR_SCRIPT_OP_SET_PUSH_CONSTANT,
        VR_SCRIPT_OP_SET_BUFFER_SUBDATA,
        VR_SCRIPT_OP_SET_IMAGE_COLOR,
        VR_SCRIPT_OP_CLEAR
};

enum vr_script_source_type {
        VR_SCRIPT_SOURCE_TYPE_GLSL,
        VR_SCRIPT_SOURCE_TYPE_SPIRV,
        VR_SCRIPT_SOURCE_TYPE_BINARY
};

enum vr_script_image_color {
        VR_SCRIPT_IMAGE_COLOR_RED,
        VR_SCRIPT_IMAGE_COLOR_GREEN,
        VR_SCRIPT_IMAGE_COLOR_BLUE,
        VR_SCRIPT_IMAGE_COLOR_RGBW,
};

struct vr_script_shader {
        struct vr_list link;
        enum vr_script_source_type source_type;
        size_t length;
        char source[];
};

struct vr_script_command {
        enum vr_script_op op;
        int line_num;

        union {
                struct {
                        float x, y, w, h;
                        unsigned pipeline_key;
                } draw_rect;

                struct {
                        unsigned x, y, z;
                } dispatch_compute;

                struct {
                        int n_components;
                        int x, y, w, h;
                        double color[4];
                } probe_rect;

                struct {
                        unsigned desc_set;
                        unsigned binding;
                        enum vr_box_comparison comparison;
                        size_t offset;
                        struct vr_box value;
                } probe_ssbo;

                struct {
                        unsigned desc_set;
                        unsigned binding;
                        size_t offset;
                        size_t size;
                        void *data;
                } set_buffer_subdata;

                struct {
                        size_t offset;
                        size_t size;
                        void *data;
                } set_push_constant;

                struct {
                        float color[4];
                        float depth;
                        uint32_t stencil;
                } clear;

                struct {
                        VkPrimitiveTopology topology;
                        bool indexed;
                        uint32_t vertex_count;
                        uint32_t instance_count;
                        uint32_t first_vertex;
                        uint32_t first_instance;
                        unsigned pipeline_key;
                } draw_arrays;

                struct {
                        unsigned desc_set;
                        unsigned binding;
                        size_t width;
                        size_t height;
                        enum vr_script_image_color color;
                } set_image_color;
        };
};

enum vr_script_resource_type {
        VR_SCRIPT_RESOURCE_TYPE_SAMPLER,
        VR_SCRIPT_RESOURCE_TYPE_COMBINED_IMAGE,
        VR_SCRIPT_RESOURCE_TYPE_SAMPLED_IMAGE,
        VR_SCRIPT_RESOURCE_TYPE_STORAGE_IMAGE,
        VR_SCRIPT_RESOURCE_TYPE_UNIFORM_TEXEL,
        VR_SCRIPT_RESOURCE_TYPE_STORAGE_TEXEL,
        VR_SCRIPT_RESOURCE_TYPE_UNIFORM_BUFFER,
        VR_SCRIPT_RESOURCE_TYPE_STORAGE_BUFFER,
        VR_SCRIPT_RESOURCE_N_TYPES,
};

struct vr_script_resource {
        unsigned desc_set;
        unsigned binding;
        enum vr_script_resource_type type;
        union {
                struct {
                        size_t size;
                } buffer;

                /* VR_SCRIPT_RESOURCE_TYPE_SAMPLER resource uses only |sampler|
                 * of |struct image|. VR_SCRIPT_RESOURCE_TYPE_SAMPLED_IMAGE
                 * and VR_SCRIPT_RESOURCE_TYPE_STORAGE_IMAGE resources uses only
                 * |format|, |width|, and |height|.
                 * VR_SCRIPT_RESOURCE_TYPE_COMBINED_IMAGE uses all of them. */
                struct {
                        const struct vr_format *format;
                        size_t width;
                        size_t height;
                        VkSamplerCreateInfo sampler;
                } image;
        };
};

struct vr_script {
        char *filename;
        struct vr_list stages[VR_SCRIPT_N_STAGES];
        size_t n_commands;
        struct vr_script_command *commands;
        size_t n_pipeline_keys;
        struct vr_pipeline_key *pipeline_keys;
        VkPhysicalDeviceFeatures required_features;
        const char *const *extensions;
        const struct vr_format *framebuffer_format;
        const struct vr_format *depth_stencil_format;
        struct vr_vbo *vertex_data;
        uint16_t *indices;
        size_t n_indices;
        struct vr_script_resource *resources;
        size_t n_resources;
};

struct vr_script *
vr_script_load_from_file(const struct vr_config *config,
                         const char *filename);

struct vr_script *
vr_script_load_from_string(const struct vr_config *config,
                           const char *string,
                           const char *tag);

void
vr_script_free(struct vr_script *script);

#endif /* VR_SCRIPT_H */
