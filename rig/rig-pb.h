/*
 * Rig
 *
 * UI Engine & Editor
 *
 * Copyright (C) 2013  Intel Corporation
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

#ifndef __RIG_PB_H__
#define __RIG_PB_H__

#include <rut.h>

typedef struct _rig_pb_serializer_t rig_pb_serializer_t;
typedef struct _rig_pb_un_serializer_t rig_pb_un_serializer_t;

#include "rig-engine.h"
#include "rig-ui.h"
#include "rig.pb-c.h"
#include "rig-entity.h"

#include "components/rig-camera.h"
#include "components/rig-light.h"

typedef bool (*rig_pb_asset_filter_t)(rig_asset_t *asset, void *user_data);

typedef uint64_t (*rig_pb_serializer_objec_register_callback_t)(
    void *object, void *user_data);

typedef uint64_t (*rig_pb_serializer_objec_to_id_callback_t)(void *object,
                                                             void *user_data);

struct _rig_pb_serializer_t {
    rig_engine_t *engine;

    rut_memory_stack_t *stack;

    rig_pb_asset_filter_t asset_filter;
    void *asset_filter_data;

    rig_pb_serializer_objec_register_callback_t object_register_callback;
    void *object_register_data;

    rig_pb_serializer_objec_to_id_callback_t object_to_id_callback;
    void *object_to_id_data;

    bool only_asset_ids;
    c_list_t *required_assets;

    bool skip_image_data;

    int n_pb_entities;
    c_list_t *pb_entities;

    int n_pb_components;
    c_list_t *pb_components;

    int n_pb_properties;
    c_list_t *pb_properties;

    int n_properties;
    void **properties_out;

    int next_id;
    c_hash_table_t *object_to_id_map;
};

typedef void (*pb_message_init_func_t)(void *message);

static inline void *
_rig_pb_new(rig_pb_serializer_t *serializer,
            size_t size,
            size_t alignment,
            void *_message_init)
{
    rut_memory_stack_t *stack = serializer->stack;
    pb_message_init_func_t message_init = _message_init;

    void *msg = rut_memory_stack_memalign(stack, size, alignment);
    message_init(msg);
    return msg;
}

#define rig_pb_new(serializer, type, init)                                     \
    _rig_pb_new(serializer, sizeof(type), RUT_UTIL_ALIGNOF(type), init)

static inline void *
_rig_pb_dup(rig_pb_serializer_t *serializer,
            size_t size,
            size_t alignment,
            void *_message_init,
            void *src)
{
    rut_memory_stack_t *stack = serializer->stack;
    pb_message_init_func_t message_init = _message_init;

    void *msg = rut_memory_stack_memalign(stack, size, alignment);
    message_init(msg);

    memcpy(msg, src, size);

    return msg;
}

#define rig_pb_dup(serializer, type, init, src)                                \
    _rig_pb_dup(serializer, sizeof(type), RUT_UTIL_ALIGNOF(type), init, src)

const char *rig_pb_strdup(rig_pb_serializer_t *serializer, const char *string);

typedef void (*rig_asset_reference_callback_t)(rig_asset_t *asset,
                                               void *user_data);

rig_pb_serializer_t *rig_pb_serializer_new(rig_engine_t *engine);

void rig_pb_serializer_set_stack(rig_pb_serializer_t *serializer,
                                 rut_memory_stack_t *stack);

void
rig_pb_serializer_set_use_pointer_ids_enabled(rig_pb_serializer_t *serializer,
                                              bool use_pointers);

void rig_pb_serializer_set_asset_filter(rig_pb_serializer_t *serializer,
                                        rig_pb_asset_filter_t filter,
                                        void *user_data);

void
rig_pb_serializer_set_only_asset_ids_enabled(rig_pb_serializer_t *serializer,
                                             bool only_ids);

void rig_pb_serializer_set_object_register_callback(
    rig_pb_serializer_t *serializer,
    rig_pb_serializer_objec_register_callback_t callback,
    void *user_data);

void rig_pb_serializer_set_object_to_id_callback(
    rig_pb_serializer_t *serializer,
    rig_pb_serializer_objec_to_id_callback_t callback,
    void *user_data);

void rig_pb_serializer_set_skip_image_data(rig_pb_serializer_t *serializer,
                                           bool skip);

uint64_t rig_pb_serializer_register_object(rig_pb_serializer_t *serializer,
                                           void *object);

uint64_t rig_pb_serializer_lookup_object_id(rig_pb_serializer_t *serializer,
                                            void *object);

void rig_pb_serializer_destroy(rig_pb_serializer_t *serializer);

Rig__UI *rig_pb_serialize_ui(rig_pb_serializer_t *serializer,
                             bool play_mode,
                             rig_ui_t *ui);

Rig__Entity *rig_pb_serialize_entity(rig_pb_serializer_t *serializer,
                                     rig_entity_t *parent,
                                     rig_entity_t *entity);

Rig__Entity__Component *
rig_pb_serialize_component(rig_pb_serializer_t *serializer,
                           rut_component_t *component);

Rig__Controller *rig_pb_serialize_controller(rig_pb_serializer_t *serializer,
                                             rig_controller_t *controller);

void rig_pb_serialized_ui_destroy(Rig__UI *ui);

Rig__Event **rig_pb_serialize_input_events(rig_pb_serializer_t *serializer,
                                           rut_input_queue_t *input_queue);

void rig_pb_property_value_init(rig_pb_serializer_t *serializer,
                                Rig__PropertyValue *pb_value,
                                const rut_boxed_t *value);

Rig__Operation **rig_pb_serialize_ops_queue(rig_pb_serializer_t *serializer,
                                            rut_queue_t *ops);

Rig__PropertyValue *pb_property_value_new(rig_pb_serializer_t *serializer,
                                          const rut_boxed_t *value);

typedef void (*rig_pb_un_serializer_object_register_callback_t)(
    void *object, uint64_t id, void *user_data);

typedef void (*rig_pb_un_serializer_object_un_register_callback_t)(
    uint64_t id, void *user_data);

typedef void *(*rig_pb_un_serializer_id_to_objec_callback_t)(uint64_t id,
                                                             void *user_data);

typedef rig_asset_t *(*rig_pb_un_serializer_asset_callback_t)(
    rig_pb_un_serializer_t *unserializer,
    Rig__Asset *pb_asset,
    void *user_data);

struct _rig_pb_un_serializer_t {
    rig_engine_t *engine;

    rut_memory_stack_t *stack;

    rig_pb_un_serializer_object_register_callback_t object_register_callback;
    void *object_register_data;

    rig_pb_un_serializer_object_un_register_callback_t
        object_unregister_callback;
    void *object_unregister_data;

    rig_pb_un_serializer_id_to_objec_callback_t id_to_object_callback;
    void *id_to_object_data;

    rig_pb_un_serializer_asset_callback_t unserialize_asset_callback;
    void *unserialize_asset_data;

    c_list_t *assets;
    c_list_t *entities;
    rig_entity_t *light;
    c_list_t *controllers;

    c_hash_table_t *id_to_object_map;
};

rig_pb_un_serializer_t *rig_pb_unserializer_new(rig_engine_t *engine);

void rig_pb_unserializer_set_object_register_callback(
    rig_pb_un_serializer_t *unserializer,
    rig_pb_un_serializer_object_register_callback_t callback,
    void *user_data);

void rig_pb_unserializer_set_object_unregister_callback(
    rig_pb_un_serializer_t *unserializer,
    rig_pb_un_serializer_object_un_register_callback_t callback,
    void *user_data);

void rig_pb_unserializer_set_id_to_object_callback(
    rig_pb_un_serializer_t *serializer,
    rig_pb_un_serializer_id_to_objec_callback_t callback,
    void *user_data);

void rig_pb_unserializer_set_asset_unserialize_callback(
    rig_pb_un_serializer_t *unserializer,
    rig_pb_un_serializer_asset_callback_t callback,
    void *user_data);

void rig_pb_unserializer_collect_error(rig_pb_un_serializer_t *unserializer,
                                       const char *format,
                                       ...);

void rig_pb_unserializer_register_object(rig_pb_un_serializer_t *unserializer,
                                         void *object,
                                         uint64_t id);

void rig_pb_unserializer_unregister_object(rig_pb_un_serializer_t *unserializer,
                                           uint64_t id);

void rig_pb_unserializer_destroy(rig_pb_un_serializer_t *unserializer);

rig_ui_t *rig_pb_unserialize_ui(rig_pb_un_serializer_t *unserializer,
                                const Rig__UI *pb_ui);

rut_mesh_t *rig_pb_unserialize_mesh(rig_pb_un_serializer_t *unserializer,
                                    Rig__Mesh *pb_mesh);

void rig_pb_init_boxed_value(rig_pb_un_serializer_t *unserializer,
                             rut_boxed_t *boxed,
                             rut_property_type_t type,
                             Rig__PropertyValue *pb_value);

/* Note: this will also add the component to the given entity, since
 * many components can't be configured until they are associated with
 * an entity. */
rut_object_t *
rig_pb_unserialize_component(rig_pb_un_serializer_t *unserializer,
                             rig_entity_t *entity,
                             Rig__Entity__Component *pb_component);

rig_entity_t *rig_pb_unserialize_entity(rig_pb_un_serializer_t *unserializer,
                                        Rig__Entity *pb_entity);

rig_controller_t *
rig_pb_unserialize_controller_bare(rig_pb_un_serializer_t *unserializer,
                                   Rig__Controller *pb_controller);

void rig_pb_unserialize_controller_properties(
    rig_pb_un_serializer_t *unserializer,
    rig_controller_t *controller,
    int n_properties,
    Rig__Controller__Property **properties);

#endif /* __RIG_PB_H__ */
