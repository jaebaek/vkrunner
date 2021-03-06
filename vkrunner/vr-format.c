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

#include "vr-error-message.h"
#include "vr-format.h"
#include "vr-hex.h"
#include "vr-util.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include "vr-format-table.h"

const struct vr_format *
vr_format_lookup_by_name(const char *name)
{
        for (int i = 0; i < VR_N_ELEMENTS(formats); i++) {
                if (!vr_strcasecmp(formats[i].name, name))
                        return formats + i;
        }

        return NULL;
}

const struct vr_format *
vr_format_lookup_by_vk_format(VkFormat vk_format)
{
        for (int i = 0; i < VR_N_ELEMENTS(formats); i++) {
                if (formats[i].vk_format == vk_format)
                        return formats + i;
        }

        return NULL;
}

const struct vr_format *
vr_format_lookup_by_details(int bit_size,
                            enum vr_format_mode mode,
                            int n_components)
{
        static const enum vr_format_component comp_order[] = {
                VR_FORMAT_COMPONENT_R,
                VR_FORMAT_COMPONENT_G,
                VR_FORMAT_COMPONENT_B,
                VR_FORMAT_COMPONENT_A,
        };

        for (int i = 0; i < VR_N_ELEMENTS(formats); i++) {
                if (formats[i].n_parts != n_components ||
                    formats[i].packed_size != 0)
                        continue;

                for (int j = 0; j < n_components; j++) {
                        if (formats[i].parts[j].bits != bit_size ||
                            formats[i].parts[j].component != comp_order[j] ||
                            formats[i].parts[j].mode != mode)
                                goto bad_format;
                }

                return formats + i;

        bad_format:
                continue;
        }

        return NULL;
}

int
vr_format_get_size(const struct vr_format *format)
{
        if (format->packed_size)
                return format->packed_size / 8;

        int total_size = 0;

        for (int i = 0; i < format->n_parts; i++)
                total_size += format->parts[i].bits;

        return total_size / 8;
}

static int32_t
sign_extend(uint32_t part, int bits)
{
        if (part & (1 << (bits - 1)))
                return (UINT32_MAX << bits) | part;
        else
                return part;
}

static double
load_packed_part(uint32_t part,
                 int bits,
                 enum vr_format_mode mode)
{
        assert(bits < 32);

        switch (mode) {
        case VR_FORMAT_MODE_SRGB:
        case VR_FORMAT_MODE_UNORM:
                return part / (double) ((1 << bits) - 1);
        case VR_FORMAT_MODE_SNORM:
                return (sign_extend(part, bits) /
                        (double) ((1 << (bits - 1)) - 1));
        case VR_FORMAT_MODE_UINT:
        case VR_FORMAT_MODE_USCALED:
                return part;
        case VR_FORMAT_MODE_SSCALED:
        case VR_FORMAT_MODE_SINT:
                return sign_extend(part, bits);
        case VR_FORMAT_MODE_UFLOAT:
                vr_fatal("FIXME: load from packed UFLOAT format");
        case VR_FORMAT_MODE_SFLOAT:
                vr_fatal("Unexpected packed SFLOAT format");
        }

        vr_fatal("Unknown packed format");
}

static void
load_packed_parts(const struct vr_format *format,
                  const uint8_t *fb,
                  double *parts)
{
        uint64_t packed_parts;

        switch (format->packed_size) {
        case 8:
                packed_parts = *fb;
                break;
        case 16:
                packed_parts = *(uint16_t *) fb;
                break;
        case 32:
                packed_parts = *(uint32_t *) fb;
                break;
        default:
                vr_fatal("Unknown packed bit size: %i", format->packed_size);
        }

        for (int i = format->n_parts - 1; i >= 0; i--) {
                int bits = format->parts[i].bits;
                uint32_t part = packed_parts & ((1 << bits) - 1);

                parts[i] = load_packed_part(part, bits, format->parts[i].mode);
                packed_parts >>= bits;
        }
}

static double
load_part(int bits,
          const uint8_t *fb,
          enum vr_format_mode mode)
{
        switch (mode) {
        case VR_FORMAT_MODE_SRGB:
        case VR_FORMAT_MODE_UNORM:
                switch (bits) {
                case 8:
                        return *fb / (double) UINT8_MAX;
                case 16:
                        return (*(uint16_t *) fb) / (double) UINT16_MAX;
                case 32:
                        return (*(uint32_t *) fb) / (double) UINT32_MAX;
                case 64:
                        return (*(uint64_t *) fb) / (double) UINT64_MAX;
                }
                break;
        case VR_FORMAT_MODE_SNORM:
                switch (bits) {
                case 8:
                        return (*(int8_t *) fb) / (double) INT8_MAX;
                case 16:
                        return (*(int16_t *) fb) / (double) INT16_MAX;
                case 32:
                        return (*(int32_t *) fb) / (double) INT32_MAX;
                case 64:
                        return (*(int64_t *) fb) / (double) INT64_MAX;
                }
                break;
        case VR_FORMAT_MODE_UINT:
        case VR_FORMAT_MODE_USCALED:
                switch (bits) {
                case 8:
                        return *fb;
                case 16:
                        return *(uint16_t *) fb;
                case 32:
                        return *(uint32_t *) fb;
                case 64:
                        return *(uint64_t *) fb;
                }
                break;
        case VR_FORMAT_MODE_SINT:
        case VR_FORMAT_MODE_SSCALED:
                switch (bits) {
                case 8:
                        return *(int8_t *) fb;
                case 16:
                        return *(int16_t *) fb;
                case 32:
                        return *(int32_t *) fb;
                case 64:
                        return *(int64_t *) fb;
                }
                break;
        case VR_FORMAT_MODE_UFLOAT:
                break;
        case VR_FORMAT_MODE_SFLOAT:
                switch (bits) {
                case 16:
                        vr_fatal("FIXME: load pixel from half-float format");
                case 32:
                        return *(float *) fb;
                case 64:
                        return *(double *) fb;
                }
                break;
        }

        vr_fatal("Unknown format bit size combination");
}

void
vr_format_load_pixel(const struct vr_format *format,
                     const uint8_t *p,
                     double *pixel)
{
        for (int i = 0; i < 3; i++)
                pixel[i] = 0.0;
        /* Alpha component defaults to 1.0 if not contained in the format */
        pixel[3] = 1.0f;

        double parts[4];

        if (format->packed_size) {
                load_packed_parts(format, p, parts);
        } else {
                for (int i = 0; i < format->n_parts; i++) {
                        int bits = format->parts[i].bits;
                        parts[i] = load_part(bits, p, format->parts[i].mode);
                        p += bits / 8;
                }
        }

        for (int i = 0; i < format->n_parts; i++) {
                switch (format->parts[i].component) {
                case VR_FORMAT_COMPONENT_R:
                        pixel[0] = parts[i];
                        break;
                case VR_FORMAT_COMPONENT_G:
                        pixel[1] = parts[i];
                        break;
                case VR_FORMAT_COMPONENT_B:
                        pixel[2] = parts[i];
                        break;
                case VR_FORMAT_COMPONENT_A:
                        pixel[3] = parts[i];
                        break;
                case VR_FORMAT_COMPONENT_D:
                case VR_FORMAT_COMPONENT_S:
                case VR_FORMAT_COMPONENT_X:
                        break;
                }
        }
}

/**
 * Parse a single number (floating point or integral) from one of the
 * data rows, and store it in the location pointed to by \c data.
 * Update \c text to point to the next character of input.
 *
 * If there is a parse failure, print a description of the problem and
 * then return false.  Otherwise return true.
 */
bool
vr_format_parse_datum(const struct vr_config *config,
                      enum vr_format_mode mode,
                      int bit_size,
                      const char **text,
                      void *data)
{
        char *endptr;
        errno = 0;
        switch (mode) {
        case VR_FORMAT_MODE_SFLOAT:
                switch (bit_size) {
                case 16: {
                        unsigned short value = vr_hex_strtohf(*text, &endptr);
                        if (errno == ERANGE) {
                                vr_error_message(config,
                                                 "Could not parse as "
                                                 "half float");
                                return false;
                        }
                        *((uint16_t *) data) = value;
                        goto handled;
                }
                case 32: {
                        float value = vr_hex_strtof(*text, &endptr);
                        if (errno == ERANGE) {
                                vr_error_message(config,
                                                 "Could not parse as float");
                                return false;
                        }
                        *((float *) data) = value;
                        goto handled;
                }
                case 64: {
                        double value = vr_hex_strtod(*text, &endptr);
                        if (errno == ERANGE) {
                                vr_error_message(config,
                                                 "Could not parse as double");
                                return false;
                        }
                        *((double *) data) = value;
                        goto handled;
                }
                }
                break;
        case VR_FORMAT_MODE_UNORM:
        case VR_FORMAT_MODE_USCALED:
        case VR_FORMAT_MODE_UINT:
        case VR_FORMAT_MODE_SRGB:
                switch (bit_size) {
                case 8: {
                        unsigned long value = strtoul(*text, &endptr, 0);
                        if (errno == ERANGE || value > UINT8_MAX) {
                                vr_error_message(config,
                                                 "Could not parse as unsigned "
                                                 "byte");
                                return false;
                        }
                        *((uint8_t *) data) = (uint8_t) value;
                        goto handled;
                }
                case 16: {
                        unsigned long value = strtoul(*text, &endptr, 0);
                        if (errno == ERANGE || value > UINT16_MAX) {
                                vr_error_message(config,
                                                 "Could not parse as unsigned "
                                                 "short");
                                return false;
                        }
                        *((uint16_t *) data) = (uint16_t) value;
                        goto handled;
                }
                case 32: {
                        unsigned long value = strtoul(*text, &endptr, 0);
                        if (errno == ERANGE || value > UINT32_MAX) {
                                vr_error_message(config,
                                                 "Could not parse as "
                                                 "unsigned integer");
                                return false;
                        }
                        *((uint32_t *) data) = (uint32_t) value;
                        goto handled;
                }
                case 64: {
                        unsigned long value = strtoul(*text, &endptr, 0);
                        if (errno == ERANGE || value > UINT64_MAX) {
                                vr_error_message(config,
                                                 "Could not parse as "
                                                 "unsigned long");
                                return false;
                        }
                        *((uint64_t *) data) = (uint64_t) value;
                        goto handled;
                }
                }
                break;
        case VR_FORMAT_MODE_SNORM:
        case VR_FORMAT_MODE_SSCALED:
        case VR_FORMAT_MODE_SINT:
                switch (bit_size) {
                case 8: {
                        long value = strtol(*text, &endptr, 0);
                        if (errno == ERANGE ||
                            value > INT8_MAX || value < INT8_MIN) {
                                vr_error_message(config,
                                                 "Could not parse as signed "
                                                 "byte");
                                return false;
                        }
                        *((int8_t *) data) = (int8_t) value;
                        goto handled;
                }
                case 16: {
                        long value = strtol(*text, &endptr, 0);
                        if (errno == ERANGE ||
                            value > INT16_MAX || value < INT16_MIN) {
                                vr_error_message(config,
                                                 "Could not parse as signed "
                                                 "short");
                                return false;
                        }
                        *((int16_t *) data) = (int16_t) value;
                        goto handled;
                }
                case 32: {
                        long value = strtol(*text, &endptr, 0);
                        if (errno == ERANGE ||
                            value > INT32_MAX || value < INT32_MIN) {
                                vr_error_message(config,
                                                 "Could not parse as "
                                                 "signed integer");
                                return false;
                        }
                        *((int32_t *) data) = (int32_t) value;
                        goto handled;
                }
                case 64: {
                        long value = strtol(*text, &endptr, 0);
                        if (errno == ERANGE ||
                            value > INT64_MAX || value < INT64_MIN) {
                                vr_error_message(config,
                                                 "Could not parse as "
                                                 "signed long");
                                return false;
                        }
                        *((int64_t *) data) = (int64_t) value;
                        goto handled;
                }
                }
                break;
        case VR_FORMAT_MODE_UFLOAT:
                break;
        }

        vr_fatal("Unexpected format");

handled:
        *text = endptr;

        return true;
}
