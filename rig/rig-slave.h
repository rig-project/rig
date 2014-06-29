/*
 * Rig
 *
 * UI Engine & Editor
 *
 * Copyright (C) 2014  Intel Corporation.
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

#ifndef _RIG_SLAVE_H
#define _RIG_SLAVE_H

#include <rut.h>

#include "rig-frontend.h"

typedef struct _rig_slave_t {
    rut_object_base_t _base;

    rut_shell_t *shell;
    rut_context_t *ctx;

    int request_width;
    int request_height;
    int request_scale;

    rig_frontend_t *frontend;
    rig_engine_t *engine;

    GHashTable *edit_id_to_play_object_map;
    GHashTable *play_object_to_edit_id_map;

    rig_pb_un_serializer_t *ui_unserializer;

    rig_engine_op_map_context_t map_op_ctx;
    rig_engine_op_apply_context_t apply_op_ctx;

    rut_closure_t *ui_update_closure;
    rut_queue_t *pending_edits;

    rut_closure_t *ui_load_closure;
    const Rig__UI *pending_ui_load;
    Rig__LoadResult_Closure pending_ui_load_closure;
    void *pending_ui_load_closure_data;

} rig_slave_t;

rig_slave_t *rig_slave_new(int width, int height, int scale);

void rig_slave_run(rig_slave_t *slave);

void rig_slave_print_mappings(rig_slave_t *slave);

#endif /* _RIG_SLAVE_H */
