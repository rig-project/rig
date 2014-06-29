/*
 * Rig
 *
 * UI Engine & Editor
 *
 * Copyright (C) 2013 Intel Corporation.
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
 *
 */

#ifndef __RIG_SLAVE_MASTER__
#define __RIG_SLAVE_MASTER__

#include <clib.h>

#include "protobuf-c-rpc/rig-protobuf-c-rpc.h"

#include "rig-slave-address.h"
#include "rig-rpc-network.h"
#include "rig-engine.h"

typedef struct _rig_slave_master_t {
    rut_object_base_t _base;

    rig_engine_t *engine;

    rig_slave_address_t *slave_address;
    rig_rpc_client_t *rpc_client;
    bool connected;
    GHashTable *registry;

} rig_slave_master_t;

void rig_connect_to_slave(rig_engine_t *engine,
                          rig_slave_address_t *slave_address);

void rig_slave_master_reload_ui(rig_slave_master_t *master);

void rig_slave_master_forward_pb_ui_edit(rig_slave_master_t *master,
                                         Rig__UIEdit *pb_edit);

#endif /* __RIG_SLAVE_MASTER__ */
