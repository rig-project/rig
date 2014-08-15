/*
 * Rut
 *
 * Rig Utilities
 *
 * Copyright (C) 2013,2014  Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _RUT_CONTEXT_H_
#define _RUT_CONTEXT_H_

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#ifdef USE_PANGO
#include <cogl-pango/cogl-pango.h>
#endif

#include "rut-object.h"
#include "rut-shell.h"
#include "rut-property.h"
#include "rut-closure.h"
#include "rut-matrix-stack.h"

/* TODO: This header needs to be split up, since most of the APIs here
 * don't relate directly to the rut_context_t type. */

#define RUT_UINT32_RED_AS_FLOAT(COLOR) (((COLOR & 0xff000000) >> 24) / 255.0)
#define RUT_UINT32_GREEN_AS_FLOAT(COLOR) (((COLOR & 0xff0000) >> 16) / 255.0)
#define RUT_UINT32_BLUE_AS_FLOAT(COLOR) (((COLOR & 0xff00) >> 8) / 255.0)
#define RUT_UINT32_ALPHA_AS_FLOAT(COLOR) ((COLOR & 0xff) / 255.0)

extern uint8_t _rut_nine_slice_indices_data[54];

/*
 * Note: The size and padding for this circle texture have been carefully
 * chosen so it has a power of two size and we have enough padding to scale
 * down the circle to a size of 2 pixels and still have a 1 texel transparent
 * border which we rely on for anti-aliasing.
 */
#define CIRCLE_TEX_RADIUS 256
#define CIRCLE_TEX_PADDING 256

typedef enum {
    RUT_TEXT_DIRECTION_LEFT_TO_RIGHT = 1,
    RUT_TEXT_DIRECTION_RIGHT_TO_LEFT
} rut_text_direction_t;

typedef struct _rut_settings_t rut_settings_t;

/* TODO Make internals private */
struct _rut_context_t {
    rut_object_base_t _base;

    /* If true then this process does not handle input events directly
     * or output graphics directly. */
    bool headless;

    rut_shell_t *shell;

    rut_settings_t *settings;

    rut_matrix_entry_t identity_entry;

    cg_device_t *cg_device;

    FcConfig *fc_config;
    FT_Library ft_library;

    cg_matrix_t identity_matrix;

    char *assets_location;

    c_hash_table_t *texture_cache;

    cg_indices_t *nine_slice_indices;

    cg_texture_t *circle_texture;

    c_hash_table_t *colors_hash;

#ifdef USE_PANGO
    CgPangoFontMap *pango_font_map;
    PangoContext *pango_context;
    PangoFontDescription *pango_font_desc;
#endif

    rut_property_context_t property_ctx;

    cg_pipeline_t *single_texture_2d_template;

    c_slist_t *timelines;
};

C_BEGIN_DECLS

rut_context_t *rut_context_new(rut_shell_t *shell /* optional */);

void rut_context_init(rut_context_t *context);

rut_text_direction_t rut_get_text_direction(rut_context_t *context);

void rut_set_assets_location(rut_context_t *context,
                             const char *assets_location);

typedef void (*rut_settings_changed_callback_t)(rut_settings_t *settings,
                                                void *user_data);

void rut_settings_add_changed_callback(rut_settings_t *settings,
                                       rut_settings_changed_callback_t callback,
                                       c_destroy_func_t destroy_notify,
                                       void *user_data);

void
rut_settings_remove_changed_callback(rut_settings_t *settings,
                                     rut_settings_changed_callback_t callback);

unsigned int rut_settings_get_password_hint_time(rut_settings_t *settings);

char *rut_settings_get_font_name(rut_settings_t *settings);

cg_texture_t *
rut_load_texture(rut_context_t *ctx, const char *filename, c_error_t **error);

cg_texture_t *rut_load_texture_from_data_file(rut_context_t *ctx,
                                              const char *filename,
                                              c_error_t **error);

char *rut_find_data_file(const char *base_filename);

cg_texture_t *
_rut_load_texture(rut_context_t *ctx, const char *filename, cg_error_t **error);

void rut_init_tls_state(void);

C_END_DECLS

#endif /* _RUT_CONTEXT_H_ */
