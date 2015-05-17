/*
 * Rig
 *
 * UI Engine & Editor
 *
 * Copyright (C) 2012  Intel Corporation
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

#include <config.h>

#include <rut.h>

/* XXX: this is actually in the rig/ directory and we need to
 * rename it... */
#include "rut-renderer.h"

#include "rig-engine.h"
#include "rig-renderer.h"

#include "components/rig-camera.h"
#include "components/rig-diamond.h"
#include "components/rig-hair.h"
#include "components/rig-light.h"
#include "components/rig-material.h"
#include "components/rig-model.h"
#include "components/rig-nine-slice.h"
#include "components/rig-pointalism-grid.h"
#include "components/rig-shape.h"

struct _rig_renderer_t {
    rut_object_base_t _base;

    rig_engine_t *engine;

    /* shadow mapping */
    cg_offscreen_t *shadow_fb;
    cg_texture_2d_t *shadow_color;
    cg_texture_t *shadow_map;

    cg_texture_t *gradient;

    cg_pipeline_t *dof_pipeline_template;
    cg_pipeline_t *dof_pipeline;
    cg_pipeline_t *dof_diamond_pipeline;
    cg_pipeline_t *dof_unshaped_pipeline;

    rig_depth_of_field_t *dof;

    rig_camera_t *composite_camera;

    cg_snippet_t *alpha_mask_snippet;
    cg_snippet_t *alpha_mask_video_snippet;
    cg_snippet_t *lighting_vertex_snippet;
    cg_snippet_t *normal_map_vertex_snippet;
    cg_snippet_t *shadow_mapping_vertex_snippet;
    cg_snippet_t *blended_discard_snippet;
    cg_snippet_t *unblended_discard_snippet;
    cg_snippet_t *premultiply_snippet;
    cg_snippet_t *unpremultiply_snippet;
    cg_snippet_t *normal_map_fragment_snippet;
    cg_snippet_t *normal_map_video_snippet;
    cg_snippet_t *material_lighting_snippet;
    cg_snippet_t *simple_lighting_snippet;
    cg_snippet_t *shadow_mapping_fragment_snippet;
    cg_snippet_t *pointalism_vertex_snippet;
    cg_snippet_t *pointalism_video_snippet;
    cg_snippet_t *pointalism_halo_snippet;
    cg_snippet_t *pointalism_opaque_snippet;
    cg_snippet_t *cache_position_snippet;
    cg_snippet_t *hair_simple_snippet;
    cg_snippet_t *hair_material_snippet;
    cg_snippet_t *hair_vertex_snippet;
    cg_snippet_t *hair_fin_snippet;

    c_array_t *journal;
};

typedef enum _cache_slot_t {
    CACHE_SLOT_SHADOW,
    CACHE_SLOT_COLOR_BLENDED,
    CACHE_SLOT_COLOR_UNBLENDED,
    CACHE_SLOT_HAIR_FINS_BLENDED,
    CACHE_SLOT_HAIR_FINS_UNBLENDED,
} cache_slot_t;

typedef enum _source_type_t {
    SOURCE_TYPE_COLOR,
    SOURCE_TYPE_ALPHA_MASK,
    SOURCE_TYPE_NORMAL_MAP
} source_type_t;

typedef struct _rig_journal_entry_t {
    rig_entity_t *entity;
    cg_matrix_t matrix;
} rig_journal_entry_t;

typedef enum _get_pipeline_flags_t {
    GET_PIPELINE_FLAG_N_FLAGS
} get_pipeline_flags_t;

/* In the shaders, any alpha value greater than or equal to this is
 * considered to be fully opaque. We can't just compare for equality
 * against 1.0 because at least on a Mac Mini there seems to be some
 * fuzziness in the interpolation of the alpha value across the
 * primitive so that it is sometimes slightly less than 1.0 even
 * though all of the vertices in the triangle are 1.0. This means some
 * of the pixels of the geometry would be painted with the blended
 * pipeline. The blended pipeline doesn't write to the depth value so
 * depending on the order of the triangles within the model it might
 * paint the back or the front of the model which causes weird sparkly
 * artifacts.
 *
 * I think it doesn't really make sense to paint models that have any
 * depth using the blended pipeline. In that case you would also need
 * to sort individual triangles of the model according to depth.
 * Perhaps the real solution to this problem is to avoid using the
 * blended pipeline at all for 3D models.
 *
 * However even for flat quad shapes it is probably good to have this
 * threshold because if a pixel is close enough to opaque that the
 * appearance will be the same then it is chaper to render it without
 * blending.
 */
#define OPAQUE_THRESHOLD 0.9999

#define N_PIPELINE_CACHE_SLOTS 5
#define N_IMAGE_SOURCE_CACHE_SLOTS 3
#define N_PRIMITIVE_CACHE_SLOTS 1

typedef struct _rig_renderer_priv_t {
    rig_renderer_t *renderer;

    cg_pipeline_t *pipeline_caches[N_PIPELINE_CACHE_SLOTS];
    rig_image_source_t *image_source_caches[N_IMAGE_SOURCE_CACHE_SLOTS];
    cg_primitive_t *primitive_caches[N_PRIMITIVE_CACHE_SLOTS];

    rut_closure_t *pointalism_grid_update_closure;
    rut_closure_t *preferred_size_closure;

    /* FIXME: these are mutually exclusive, so collapse into one pointer */
    rut_closure_t *nine_slice_closure;
    rut_closure_t *diamond_closure;
    rut_closure_t *reshaped_closure;
} rig_renderer_priv_t;

static void
_rig_renderer_free(void *object)
{
    rig_renderer_t *renderer = object;

    c_array_free(renderer->journal, true);
    renderer->journal = NULL;

    rut_object_free(rig_renderer_t, object);
}

static void
set_entity_pipeline_cache(rig_entity_t *entity,
                          int slot,
                          cg_pipeline_t *pipeline)
{
    rig_renderer_priv_t *priv = entity->renderer_priv;

    if (priv->pipeline_caches[slot])
        cg_object_unref(priv->pipeline_caches[slot]);

    priv->pipeline_caches[slot] = pipeline;
    if (pipeline)
        cg_object_ref(pipeline);
}

static cg_pipeline_t *
get_entity_pipeline_cache(rig_entity_t *entity, int slot)
{
    rig_renderer_priv_t *priv = entity->renderer_priv;
    return priv->pipeline_caches[slot];
}

static void
set_entity_image_source_cache(rig_entity_t *entity,
                              int slot,
                              rig_image_source_t *source)
{
    rig_renderer_priv_t *priv = entity->renderer_priv;

    if (priv->image_source_caches[slot])
        rut_object_unref(priv->image_source_caches[slot]);

    priv->image_source_caches[slot] = source;
    if (source)
        rut_object_ref(source);
}

static rig_image_source_t *
get_entity_image_source_cache(rig_entity_t *entity,
                              int slot)
{
    rig_renderer_priv_t *priv = entity->renderer_priv;

    return priv->image_source_caches[slot];
}

static void
set_entity_primitive_cache(rig_entity_t *entity,
                           int slot,
                           cg_primitive_t *primitive)
{
    rig_renderer_priv_t *priv = entity->renderer_priv;

    if (priv->primitive_caches[slot])
        cg_object_unref(priv->primitive_caches[slot]);

    priv->primitive_caches[slot] = primitive;
    if (primitive)
        cg_object_ref(primitive);
}

static cg_primitive_t *
get_entity_primitive_cache(rig_entity_t *entity,
                           int slot)
{
    rig_renderer_priv_t *priv = entity->renderer_priv;

    return priv->primitive_caches[slot];
}

static void
dirty_entity_pipelines(rig_entity_t *entity)
{
    set_entity_pipeline_cache(entity, CACHE_SLOT_COLOR_UNBLENDED, NULL);
    set_entity_pipeline_cache(entity, CACHE_SLOT_COLOR_BLENDED, NULL);
    set_entity_pipeline_cache(entity, CACHE_SLOT_SHADOW, NULL);
}

static void
dirty_entity_geometry(rig_entity_t *entity)
{
    set_entity_primitive_cache(entity, 0, NULL);
}

/* TODO: allow more fine grained discarding of cached renderer state */
static void
_rig_renderer_notify_entity_changed(rig_entity_t *entity)
{
    if (!entity->renderer_priv)
        return;

    dirty_entity_pipelines(entity);
    dirty_entity_geometry(entity);

    set_entity_image_source_cache(entity, SOURCE_TYPE_COLOR, NULL);
    set_entity_image_source_cache(entity, SOURCE_TYPE_ALPHA_MASK, NULL);
    set_entity_image_source_cache(entity, SOURCE_TYPE_NORMAL_MAP, NULL);
}

static void
_rig_renderer_free_priv(rig_entity_t *entity)
{
    rig_renderer_priv_t *priv = entity->renderer_priv;
    cg_pipeline_t **pipeline_caches = priv->pipeline_caches;
    rig_image_source_t **image_source_caches = priv->image_source_caches;
    cg_primitive_t **primitive_caches = priv->primitive_caches;
    int i;

    for (i = 0; i < N_PIPELINE_CACHE_SLOTS; i++) {
        if (pipeline_caches[i])
            cg_object_unref(pipeline_caches[i]);
    }

    for (i = 0; i < N_IMAGE_SOURCE_CACHE_SLOTS; i++) {
        if (image_source_caches[i])
            rut_object_unref(image_source_caches[i]);
    }

    for (i = 0; i < N_PRIMITIVE_CACHE_SLOTS; i++) {
        if (primitive_caches[i])
            cg_object_unref(primitive_caches[i]);
    }

    if (priv->pointalism_grid_update_closure)
        rut_closure_disconnect(priv->pointalism_grid_update_closure);

    if (priv->preferred_size_closure)
        rut_closure_disconnect(priv->preferred_size_closure);

    if (priv->nine_slice_closure)
        rut_closure_disconnect(priv->nine_slice_closure);

    if (priv->diamond_closure)
        rut_closure_disconnect(priv->diamond_closure);

    if (priv->reshaped_closure)
        rut_closure_disconnect(priv->reshaped_closure);

    c_slice_free(rig_renderer_priv_t, priv);
    entity->renderer_priv = NULL;
}

rut_type_t rig_renderer_type;

static void
_rig_renderer_init_type(void)
{

    static rut_renderer_vtable_t renderer_vtable = {
        .notify_entity_changed = _rig_renderer_notify_entity_changed,
        .free_priv = _rig_renderer_free_priv
    };

    rut_type_t *type = &rig_renderer_type;
#define TYPE rig_renderer_t

    rut_type_init(type, C_STRINGIFY(TYPE), _rig_renderer_free);
    rut_type_add_trait(type,
                       RUT_TRAIT_ID_RENDERER,
                       0, /* no implied properties */
                       &renderer_vtable);

#undef TYPE
}

rig_renderer_t *
rig_renderer_new(rig_frontend_t *frontend)
{
    rig_renderer_t *renderer = rut_object_alloc0(
        rig_renderer_t, &rig_renderer_type, _rig_renderer_init_type);

    renderer->engine = frontend->engine;

    renderer->journal = c_array_new(false, false, sizeof(rig_journal_entry_t));

    return renderer;
}

static void
rig_journal_log(c_array_t *journal,
                rig_paint_context_t *paint_ctx,
                rig_entity_t *entity,
                const cg_matrix_t *matrix)
{

    rig_journal_entry_t *entry;

    c_array_set_size(journal, journal->len + 1);
    entry = &c_array_index(journal, rig_journal_entry_t, journal->len - 1);

    entry->entity = rut_object_ref(entity);
    entry->matrix = *matrix;
}

static int
sort_entry_cb(const rig_journal_entry_t *entry0,
              const rig_journal_entry_t *entry1)
{
    float z0 = entry0->matrix.zw;
    float z1 = entry1->matrix.zw;

    /* TODO: also sort based on the state */

    if (z0 < z1)
        return -1;
    else if (z0 > z1)
        return 1;

    return 0;
}

static void
reshape_cb(rig_shape_t *shape, void *user_data)
{
    rut_componentable_props_t *componentable =
        rut_object_get_properties(shape, RUT_TRAIT_ID_COMPONENTABLE);
    rig_entity_t *entity = componentable->entity;
    dirty_entity_pipelines(entity);
}

static void
nine_slice_changed_cb(rig_nine_slice_t *nine_slice, void *user_data)
{
    rut_componentable_props_t *componentable =
        rut_object_get_properties(nine_slice, RUT_TRAIT_ID_COMPONENTABLE);
    rig_entity_t *entity = componentable->entity;
    _rig_renderer_notify_entity_changed(entity);
    dirty_entity_geometry(entity);
}

static void
diamond_changed_cb(rig_diamond_t *diamond, void *user_data)
{
    rut_componentable_props_t *componentable =
        rut_object_get_properties(diamond, RUT_TRAIT_ID_COMPONENTABLE);
    rig_entity_t *entity = componentable->entity;
    _rig_renderer_notify_entity_changed(entity);
    dirty_entity_geometry(entity);
}

/* Called if the geometry of the pointalism grid has changed... */
static void
pointalism_changed_cb(rig_pointalism_grid_t *grid, void *user_data)
{
    rut_componentable_props_t *componentable =
        rut_object_get_properties(grid, RUT_TRAIT_ID_COMPONENTABLE);
    rig_entity_t *entity = componentable->entity;

    dirty_entity_geometry(entity);
}

static void
set_focal_parameters(cg_pipeline_t *pipeline,
                     float focal_distance,
                     float depth_of_field)
{
    int location;
    float distance;

    /* I want to have the focal distance as positive when it's in front of the
     * camera (it seems more natural, but as, in OpenGL, the camera is facing
     * the negative Ys, the actual value to give to the shader has to be
     * negated */
    distance = -focal_distance;

    location = cg_pipeline_get_uniform_location(pipeline, "dof_focal_distance");
    cg_pipeline_set_uniform_float(
        pipeline, location, 1 /* n_components */, 1 /* count */, &distance);

    location = cg_pipeline_get_uniform_location(pipeline, "dof_depth_of_field");
    cg_pipeline_set_uniform_float(pipeline,
                                  location,
                                  1 /* n_components */,
                                  1 /* count */,
                                  &depth_of_field);
}

static void
init_dof_pipeline_template(rig_renderer_t *renderer)
{
    rig_engine_t *engine = renderer->engine;
    cg_pipeline_t *pipeline;
    cg_depth_state_t depth_state;
    cg_snippet_t *snippet;

    pipeline = cg_pipeline_new(engine->shell->cg_device);

    cg_pipeline_set_color_mask(pipeline, CG_COLOR_MASK_ALPHA);

    cg_pipeline_set_blend(pipeline, "RGBA=ADD(SRC_COLOR, 0)", NULL);

    cg_depth_state_init(&depth_state);
    cg_depth_state_set_test_enabled(&depth_state, true);
    cg_pipeline_set_depth_state(pipeline, &depth_state, NULL);

    snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_VERTEX,

        /* definitions */
        "uniform float dof_focal_distance;\n"
        "uniform float dof_depth_of_field;\n"
        "out float dof_blur;\n",
        //"out vec4 world_pos;\n",

        /* compute the amount of bluriness we want */
        "vec4 world_pos = cg_modelview_matrix * pos;\n"
        //"world_pos = cg_modelview_matrix * cg_position_in;\n"
        "dof_blur = 1.0 - clamp (abs (world_pos.z - dof_focal_distance) /\n"
        "                  dof_depth_of_field, 0.0, 1.0);\n");

    cg_pipeline_add_snippet(pipeline, renderer->cache_position_snippet);
    cg_pipeline_add_snippet(pipeline, snippet);
    cg_object_unref(snippet);

/* This was used to debug the focal distance and bluriness amount in the DoF
 * effect: */
#if 0
    cg_pipeline_set_color_mask (pipeline, CG_COLOR_MASK_ALL);
    snippet = cg_snippet_new (CG_SNIPPET_HOOK_FRAGMENT,
                              "in vec4 world_pos;\n"
                              "in float dof_blur;",

                              "cg_color_out = vec4(dof_blur,0,0,1);\n"
                              //"cg_color_out = vec4(1.0, 0.0, 0.0, 1.0);\n"
                              //"if (world_pos.z < -30.0) cg_color_out = vec4(0,1,0,1);\n"
                              //"if (abs (world_pos.z + 30.f) < 0.1) cg_color_out = vec4(0,1,0,1);\n"
                              "cg_color_out.a = dof_blur;\n"
                              //"cg_color_out.a = 1.0;\n"
                              );

    cg_pipeline_add_snippet (pipeline, snippet);
    cg_object_unref (snippet);
#endif

    renderer->dof_pipeline_template = pipeline;
}

static void
init_dof_diamond_pipeline(rig_renderer_t *renderer)
{
    rig_engine_t *engine = renderer->engine;
    cg_pipeline_t *dof_diamond_pipeline =
        cg_pipeline_copy(renderer->dof_pipeline_template);
    cg_snippet_t *snippet;

    cg_pipeline_set_layer_texture(dof_diamond_pipeline, 0,
                                  engine->shell->circle_texture);

    snippet = cg_snippet_new(CG_SNIPPET_HOOK_FRAGMENT,
                             /* declarations */
                             "in float dof_blur;",

                             /* post */
                             "if (cg_color_out.a <= 0.0)\n"
                             "  discard;\n"
                             "\n"
                             "cg_color_out.a = dof_blur;\n");

    cg_pipeline_add_snippet(dof_diamond_pipeline, snippet);
    cg_object_unref(snippet);

    renderer->dof_diamond_pipeline = dof_diamond_pipeline;
}

static void
init_dof_unshaped_pipeline(rig_renderer_t *renderer)
{
    cg_pipeline_t *dof_unshaped_pipeline =
        cg_pipeline_copy(renderer->dof_pipeline_template);
    cg_snippet_t *snippet;

    snippet = cg_snippet_new(CG_SNIPPET_HOOK_FRAGMENT,
                             /* declarations */
                             "in float dof_blur;",

                             /* post */
                             "if (cg_color_out.a < 0.25)\n"
                             "  discard;\n"
                             "\n"
                             "cg_color_out.a = dof_blur;\n");

    cg_pipeline_add_snippet(dof_unshaped_pipeline, snippet);
    cg_object_unref(snippet);

    renderer->dof_unshaped_pipeline = dof_unshaped_pipeline;
}

static void
init_dof_pipeline(rig_renderer_t *renderer)
{
    cg_pipeline_t *dof_pipeline =
        cg_pipeline_copy(renderer->dof_pipeline_template);
    cg_snippet_t *snippet;

    /* store the bluriness in the alpha channel */
    snippet = cg_snippet_new(CG_SNIPPET_HOOK_FRAGMENT,
                             "in float dof_blur;",
                             "cg_color_out.a = dof_blur;\n");
    cg_pipeline_add_snippet(dof_pipeline, snippet);
    cg_object_unref(snippet);

    renderer->dof_pipeline = dof_pipeline;
}

static void
create_debug_gradient(rig_renderer_t *renderer)
{
    cg_vertex_p2c4_t quad[] = { { 0,   0,   0xff, 0x00, 0x00, 0xff },
                                { 0,   200, 0x00, 0xff, 0x00, 0xff },
                                { 200, 200, 0x00, 0x00, 0xff, 0xff },
                                { 200, 0,   0xff, 0xff, 0xff, 0xff } };
    cg_offscreen_t *offscreen;
    cg_device_t *dev = renderer->engine->shell->cg_device;
    cg_primitive_t *prim =
        cg_primitive_new_p2c4(dev, CG_VERTICES_MODE_TRIANGLE_FAN, 4, quad);
    cg_pipeline_t *pipeline = cg_pipeline_new(dev);

    renderer->gradient = cg_texture_2d_new_with_size(dev, 200, 200);

    offscreen = cg_offscreen_new_with_texture(renderer->gradient);

    cg_framebuffer_orthographic(offscreen, 0, 0, 200, 200, -1, 100);
    cg_framebuffer_clear4f(offscreen, CG_BUFFER_BIT_COLOR | CG_BUFFER_BIT_DEPTH,
                           0, 0, 0, 1);
    cg_primitive_draw(prim, offscreen, pipeline);

    cg_object_unref(prim);
    cg_object_unref(offscreen);
}

static void
ensure_shadow_map(rig_renderer_t *renderer)
{
    rig_engine_t *engine = renderer->engine;
    cg_texture_2d_t *color_buffer;
    // rig_ui_t *ui = engine->edit_mode_ui ?
    //  engine->edit_mode_ui : engine->play_mode_ui;

    /*
     * Shadow mapping
     */

    /* Setup the shadow map */

    c_warn_if_fail(renderer->shadow_color == NULL);

    color_buffer = cg_texture_2d_new_with_size(engine->shell->cg_device,
                                               engine->device_width * 2,
                                               engine->device_height * 2);

    renderer->shadow_color = color_buffer;

    c_warn_if_fail(renderer->shadow_fb == NULL);

    /* XXX: Right now there's no way to avoid allocating a color buffer. */
    renderer->shadow_fb = cg_offscreen_new_with_texture(color_buffer);
    if (renderer->shadow_fb == NULL) {
        c_warning("could not create offscreen buffer");
        cg_object_unref(color_buffer);
        return;
    }

    /* retrieve the depth texture */
    cg_framebuffer_set_depth_texture_enabled(renderer->shadow_fb, true);

    c_warn_if_fail(renderer->shadow_map == NULL);

    renderer->shadow_map = cg_framebuffer_get_depth_texture(renderer->shadow_fb);

    /* Create a color gradient texture that can be used for debugging
     * shadow mapping.
     *
     * XXX: This should probably simply be #ifdef DEBUG code.
     */
    create_debug_gradient(renderer);
}

static void
free_shadow_map(rig_renderer_t *renderer)
{
    if (renderer->shadow_map) {
        cg_object_unref(renderer->shadow_map);
        renderer->shadow_map = NULL;
    }
    if (renderer->shadow_fb) {
        cg_object_unref(renderer->shadow_fb);
        renderer->shadow_fb = NULL;
    }
    if (renderer->shadow_color) {
        cg_object_unref(renderer->shadow_color);
        renderer->shadow_color = NULL;
    }
}

void
rig_renderer_init(rig_renderer_t *renderer)
{
    ensure_shadow_map(renderer);

    /* We always want to use exactly the same snippets when creating
     * similar pipelines so that we can take advantage of Cogl's program
     * caching. The program cache only compares the snippet pointers,
     * not the contents of the snippets. Therefore we just create the
     * snippets we're going to use upfront and retain them */

    renderer->alpha_mask_snippet =
        cg_snippet_new(CG_SNIPPET_HOOK_FRAGMENT,
                       /* definitions */
                       "uniform float material_alpha_threshold;\n",
                       /* post */
                       "if (rig_image_source_sample4 (\n"
                       "    cg_tex_coord4_in.st).r < \n"
                       "    material_alpha_threshold)\n"
                       "  discard;\n");

    renderer->lighting_vertex_snippet =
        cg_snippet_new(CG_SNIPPET_HOOK_VERTEX,
                       /* definitions */
                       "uniform mat3 normal_matrix;\n"
                       "in vec3 tangent_in;\n"
                       "out vec3 normal, eye_direction;\n",
                       /* post */
                       "normal = normalize(normal_matrix * cg_normal_in);\n"
                       "eye_direction = -vec3(cg_modelview_matrix *\n"
                       "                      pos);\n");

    renderer->normal_map_vertex_snippet =
        cg_snippet_new(CG_SNIPPET_HOOK_VERTEX,
                       /* definitions */
                       "uniform vec3 light0_direction_norm;\n"
                       "out vec3 light_direction;\n",

                       /* post */
                       "vec3 tangent = normalize(normal_matrix * tangent_in);\n"
                       "vec3 binormal = cross(normal, tangent);\n"

                       /* Transform the light direction into tangent space */
                       "vec3 v;\n"
                       "v.x = dot (light0_direction_norm, tangent);\n"
                       "v.y = dot (light0_direction_norm, binormal);\n"
                       "v.z = dot (light0_direction_norm, normal);\n"
                       "light_direction = normalize (v);\n"

                       /* Transform the eye direction into tangent space */
                       "v.x = dot (eye_direction, tangent);\n"
                       "v.y = dot (eye_direction, binormal);\n"
                       "v.z = dot (eye_direction, normal);\n"
                       "eye_direction = normalize (v);\n");

    renderer->cache_position_snippet =
        cg_snippet_new(CG_SNIPPET_HOOK_VERTEX_TRANSFORM,
                       "out vec4 pos;\n",
                       "pos = cg_position_in;\n");

    renderer->pointalism_vertex_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_VERTEX_TRANSFORM,
        "in vec2 cell_xy;\n"
        "in vec4 cell_st;\n"
        "uniform float scale_factor;\n"
        "uniform float z_trans;\n"
        "uniform int anti_scale;\n"
        "out vec4 av_color;\n",
        "float grey;\n"
        "av_color = rig_image_source_sample1 (vec2 (cell_st.x, cell_st.z));\n"
        "av_color += rig_image_source_sample1 (vec2 (cell_st.y, cell_st.z));\n"
        "av_color += rig_image_source_sample1 (vec2 (cell_st.y, cell_st.w));\n"
        "av_color += rig_image_source_sample1 (vec2 (cell_st.x, cell_st.w));\n"
        "av_color /= 4.0;\n"
        "grey = av_color.r * 0.2126 + av_color.g * 0.7152 + av_color.b * "
        "0.0722;\n"
        "if (anti_scale == 1)\n"
        "{"
        "  pos.xy *= scale_factor * grey;\n"
        "  pos.z += z_trans * grey;\n"
        "}"
        "else\n"
        "{"
        "  pos.xy *= scale_factor - (scale_factor * grey);\n"
        "  pos.z += z_trans - (z_trans * grey);\n"
        "}"
        "pos.x += cell_xy.x;\n"
        "pos.y += cell_xy.y;\n"
        "cg_position_out = cg_modelview_projection_matrix * pos;\n");

    renderer->shadow_mapping_vertex_snippet =
        cg_snippet_new(CG_SNIPPET_HOOK_VERTEX,

                       /* definitions */
                       "uniform mat4 light_shadow_matrix;\n"
                       "out vec4 shadow_coords;\n",

                       /* post */
                       "shadow_coords = light_shadow_matrix *\n"
                       "                pos;\n");

    renderer->blended_discard_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_FRAGMENT,
        /* definitions */
        NULL,

        /* post */
        "if (cg_color_out.a <= 0.0 ||\n"
        "    cg_color_out.a >= " C_STRINGIFY(OPAQUE_THRESHOLD) ")\n"
        "  discard;\n");

    renderer->unblended_discard_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_FRAGMENT,
        /* definitions */
        NULL,

        /* post */
        "if (cg_color_out.a < " C_STRINGIFY(OPAQUE_THRESHOLD) ")\n"
        "  discard;\n");

    renderer->premultiply_snippet =
        cg_snippet_new(CG_SNIPPET_HOOK_FRAGMENT,
                       /* definitions */
                       NULL,

                       /* post */

                       /* FIXME: Avoid premultiplying here by fiddling the
                        * blend mode instead which should be more efficient */
                       "cg_color_out.rgb *= cg_color_out.a;\n");

    renderer->unpremultiply_snippet =
        cg_snippet_new(CG_SNIPPET_HOOK_FRAGMENT,
                       /* definitions */
                       NULL,

                       /* post */

                       /* FIXME: We need to unpremultiply our colour at this
                        * point to perform lighting, but this is obviously not
                        * ideal and we should instead avoid being premultiplied
                        * at this stage by not premultiplying our textures on
                        * load for example. */
                       "cg_color_out.rgb /= cg_color_out.a;\n");

    renderer->normal_map_fragment_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_FRAGMENT,
        /* definitions */
        "uniform vec4 light0_ambient, light0_diffuse, light0_specular;\n"
        "uniform vec4 material_ambient, material_diffuse, material_specular;\n"
        "uniform float material_shininess;\n"
        "in vec3 light_direction, eye_direction;\n",

        /* post */
        "vec4 final_color;\n"
        "vec3 L = normalize(light_direction);\n"
        "vec3 N = rig_image_source_sample7(cg_tex_coord7_in.st).rgb;\n"
        "N = 2.0 * N - 1.0;\n"
        "N = normalize(N);\n"
        "vec4 ambient = light0_ambient * material_ambient;\n"
        "final_color = ambient * cg_color_out;\n"
        "float lambert = dot(N, L);\n"
        "if (lambert > 0.0)\n"
        "{\n"
        "  vec4 diffuse = light0_diffuse * material_diffuse;\n"
        "  vec4 specular = light0_specular * material_specular;\n"
        "  final_color += cg_color_out * diffuse * lambert;\n"
        "  vec3 E = normalize(eye_direction);\n"
        "  vec3 R = reflect (-L, N);\n"
        "  float specular_factor = pow (max(dot(R, E), 0.0),\n"
        "                               material_shininess);\n"
        "  final_color += specular * specular_factor;\n"
        "}\n"
        "cg_color_out.rgb = final_color.rgb;\n");

    renderer->material_lighting_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_FRAGMENT,
        /* definitions */
        "/* material lighting declarations */\n"
        "in vec3 normal, eye_direction;\n"
        "uniform vec4 light0_ambient, light0_diffuse, light0_specular;\n"
        "uniform vec3 light0_direction_norm;\n"
        "uniform vec4 material_ambient, material_diffuse, material_specular;\n"
        "uniform float material_shininess;\n",

        /* post */
        "vec4 final_color;\n"
        "vec3 L = light0_direction_norm;\n"
        "vec3 N = normalize(normal);\n"
        "vec4 ambient = light0_ambient * material_ambient;\n"
        "final_color = ambient * cg_color_out;\n"
        "float lambert = dot(N, L);\n"
        "if (lambert > 0.0)\n"
        "{\n"
        "  vec4 diffuse = light0_diffuse * material_diffuse;\n"
        "  vec4 specular = light0_specular * material_specular;\n"
        "  final_color += cg_color_out * diffuse * lambert;\n"
        "  vec3 E = normalize(eye_direction);\n"
        "  vec3 R = reflect (-L, N);\n"
        "  float specular_factor = pow (max(dot(R, E), 0.0),\n"
        "                               material_shininess);\n"
        "  final_color += specular * specular_factor;\n"
        "}\n"
        "cg_color_out.rgb = final_color.rgb;\n");

    renderer->simple_lighting_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_FRAGMENT,
        /* definitions */
        "/* simple lighting declarations */\n"
        "in vec3 normal, eye_direction;\n"
        "uniform vec4 light0_ambient, light0_diffuse, light0_specular;\n"
        "uniform vec3 light0_direction_norm;\n",

        /* post */
        "vec4 final_color;\n"
        "vec3 L = light0_direction_norm;\n"
        "vec3 N = normalize(normal);\n"
        "final_color = light0_ambient * cg_color_out;\n"
        "float lambert = dot(N, L);\n"
        "if (lambert > 0.0)\n"
        "{\n"
        "  final_color += cg_color_out * light0_diffuse * lambert;\n"
        "  vec3 E = normalize(eye_direction);\n"
        "  vec3 R = reflect (-L, N);\n"
        "  float specular = pow (max(dot(R, E), 0.0),\n"
        "                        2.);\n"
        "  final_color += light0_specular * vec4(.6, .6, .6, 1.0) *\n"
        "                 specular;\n"
        "}\n"
        "cg_color_out.rgb = final_color.rgb;\n");

    renderer->shadow_mapping_fragment_snippet =
        cg_snippet_new(CG_SNIPPET_HOOK_FRAGMENT,
                       /* declarations */
                       "in vec4 shadow_coords;\n",
                       /* post */
                       "#if __VERSION__ >= 130\n"
                       "  vec4 texel10 =\n"
                       "    texture (cg_sampler10, shadow_coords.xy);\n"
                       "#else\n"
                       "  vec4 texel10 =\n"
                       "    texture2D (cg_sampler10, shadow_coords.xy);\n"
                       "#endif\n"
                       "  float distance_from_light = texel10.r + 0.0005;\n"
                       "  float shadow = 1.0;\n"
                       "  if (distance_from_light < shadow_coords.z)\n"
                       "    shadow = 0.5;\n"
                       "  cg_color_out.rgb = shadow * cg_color_out.rgb;\n");

    renderer->pointalism_halo_snippet =
        cg_snippet_new(CG_SNIPPET_HOOK_FRAGMENT,
                       /* declarations */
                       "in vec4 av_color;\n",

                       /* post */
                       "  cg_color_out = av_color;\n"
                       "#if __VERSION__ >= 130\n"
                       "  cg_color_out *=\n"
                       "    texture (cg_sampler0, cg_tex_coord0_in.st);\n"
                       "#else\n"
                       "  cg_color_out *=\n"
                       "    texture2D (cg_sampler0, cg_tex_coord0_in.st);\n"
                       "#endif\n"
                       "  if (cg_color_out.a > 0.90 || cg_color_out.a <= 0.0)\n"
                       "    discard;\n");

    renderer->pointalism_opaque_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_FRAGMENT,
        /* declarations */
        "in vec4 av_color;\n",

        /* post */
        "  cg_color_out = av_color;\n"
        "  cg_color_out *=\n"
        "    cg_texture_lookup0 (cg_sampler0,\n"
        "                          vec4(cg_tex_coord0_in.st, 0.0, 1.0));\n"
        "  if (cg_color_out.a < 0.90)\n"
        "    discard;\n");

    renderer->hair_simple_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_FRAGMENT,
        /* declarations */
        "/* hair simple declarations */\n"
        "in vec3 normal, eye_direction;\n"
        "uniform vec4 light0_ambient, light0_diffuse, light0_specular;\n"
        "uniform vec3 light0_direction_norm;\n",
        /* post */
        "#if __VERSION__ >= 130\n"
        "  vec4 texel =\n"
        "    texture (cg_sampler11, cg_tex_coord11_in.st);\n"
        "#else\n"
        "  vec4 texel =\n"
        "    texture2D (cg_sampler11, cg_tex_coord11_in.st);\n"
        "#endif\n"
        "  cg_color_out *= texel;\n"
        "  if (cg_color_out.a < 0.9)\n"
        "    discard;\n"
        "\n"
        "  vec3 E = normalize (eye_direction);\n"
        "  vec3 L = normalize (light0_direction_norm);\n"
        "  vec3 H = normalize (L + E);\n"
        "  vec3 N = normalize (normal);\n"
        "  vec3 Ka = light0_ambient.rgb;\n"
        "  vec3 Kd = vec3 (0.0, 0.0, 0.0);\n"
        "  vec3 Ks = vec3 (0.0, 0.0, 0.0);\n"
        "  float Pd = max (0.0, dot (N, L));\n"
        "  float Ps = max (0.0, dot (N, H));\n"
        "  float u = max (0.0, dot (N, L));\n"
        "  float v = max (0.0, dot (N, H));\n"
        "\n"
        "  if (Pd > 0.0)\n"
        "    Kd = light0_diffuse.rgb * pow (1.0 - (u * u), Pd / 2.0);\n"
        "  if (Ps > 0.0)\n"
        "    Ks = light0_specular.rgb * pow (1.0 - (v * v), Ps / 2.0);\n"
        "\n"
        "  vec3 color = Ka + Kd + Ks;\n"
        "  cg_color_out.rgb *= color;\n");

    renderer->hair_material_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_FRAGMENT,
        /* declarations */
        "/* hair material declarations */\n"
        "in vec3 normal, eye_direction;\n"
        "uniform vec4 light0_ambient, light0_diffuse, light0_specular;\n"
        "uniform vec3 light0_direction_norm;\n"
        "uniform vec4 material_ambient, material_diffuse, material_specular;\n"
        "uniform float material_shininess;\n",
        /* post */
        "#if __VERSION__ >= 130\n"
        "  vec4 texel =\n"
        "    texture (cg_sampler11, cg_tex_coord11_in.st);\n"
        "#else\n"
        "  vec4 texel =\n"
        "    texture2D (cg_sampler11, cg_tex_coord11_in.st);\n"
        "#endif\n"
        "\n"
        "  cg_color_out *= texel;\n"
        "  if (cg_color_out.a < 0.9)\n"
        "    discard;\n"
        "\n"
        "  vec3 E = normalize(eye_direction);\n"
        "  vec3 L = normalize (light0_direction_norm);\n"
        "  vec3 H = normalize (L + E);\n"
        "  vec3 N = normalize (normal);\n"
        "  vec3 Ka = light0_ambient.rgb * material_ambient.rgb;\n"
        "  vec3 Kd = vec3 (0.0, 0.0, 0.0);\n"
        "  vec3 Ks = vec3 (0.0, 0.0, 0.0);\n"
        "  float Pd = max (0.0, dot (N, L));\n"
        "  float Ps = max (0.0, dot (N, H));\n"
        "  float u = max (0.0, dot (N, L));\n"
        "  float v = max (0.0, dot (N, H));\n"
        "\n"
        "  if (Pd > 0.0) {\n"
        "    Kd = (light0_diffuse.rgb * material_diffuse.rgb) *\n"
        "         pow (1.0 - (u * u), Pd / 2.0);\n"
        "  }\n"
        "  if (Ps > 0.0) {\n"
        "    Ks = (light0_specular.rgb * material_specular.rgb) *\n"
        "         pow (1.0 - (v * v), Ps / 2.0);\n"
        "  }\n"
        "\n"
        "  vec3 color = Ka + Kd + Ks;\n"
        "  cg_color_out.rgb *= color;\n");

    renderer->hair_vertex_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_VERTEX,
        /* declarations */
        "uniform float hair_pos;\n",
        /* post */
        "vec4 displace = pos;\n"
        "displace.xyz = cg_normal_in * hair_pos + displace.xyz;\n"
        "cg_position_out = cg_modelview_projection_matrix * displace;\n");

    renderer->hair_fin_snippet = cg_snippet_new(
        CG_SNIPPET_HOOK_VERTEX,
        "uniform float length;\n",
        "vec4 displace = pos;\n"
        "if (cg_tex_coord11_in.t < 1.0)\n"
        "displace.xyz += cg_normal_in * length;\n"
        "cg_position_out = cg_modelview_projection_matrix * displace;\n");

    init_dof_pipeline_template(renderer);

    init_dof_diamond_pipeline(renderer);

    init_dof_unshaped_pipeline(renderer);

    init_dof_pipeline(renderer);

    renderer->composite_camera = rig_camera_new(renderer->engine,
                                                1, 1, /* ortho w/h */
                                                NULL); /* fb */
    rut_camera_set_clear(renderer->composite_camera, false);
}

void
rig_renderer_fini(rig_renderer_t *renderer)
{
    rut_object_unref(renderer->composite_camera);

    cg_object_unref(renderer->dof_pipeline_template);
    renderer->dof_pipeline_template = NULL;

    cg_object_unref(renderer->dof_pipeline);
    renderer->dof_pipeline = NULL;

    cg_object_unref(renderer->dof_diamond_pipeline);
    renderer->dof_diamond_pipeline = NULL;

    cg_object_unref(renderer->dof_unshaped_pipeline);
    renderer->dof_unshaped_pipeline = NULL;


    if (renderer->dof) {
        rig_dof_effect_free(renderer->dof);
        renderer->dof = NULL;
    }

    cg_object_unref(renderer->alpha_mask_snippet);
    renderer->alpha_mask_snippet = NULL;

    cg_object_unref(renderer->lighting_vertex_snippet);
    renderer->lighting_vertex_snippet = NULL;

    cg_object_unref(renderer->normal_map_vertex_snippet);
    renderer->normal_map_vertex_snippet = NULL;

    cg_object_unref(renderer->shadow_mapping_vertex_snippet);
    renderer->shadow_mapping_vertex_snippet = NULL;

    cg_object_unref(renderer->blended_discard_snippet);
    renderer->blended_discard_snippet = NULL;

    cg_object_unref(renderer->unblended_discard_snippet);
    renderer->unblended_discard_snippet = NULL;

    cg_object_unref(renderer->premultiply_snippet);
    renderer->premultiply_snippet = NULL;

    cg_object_unref(renderer->unpremultiply_snippet);
    renderer->unpremultiply_snippet = NULL;

    cg_object_unref(renderer->normal_map_fragment_snippet);
    renderer->normal_map_fragment_snippet = NULL;

    cg_object_unref(renderer->material_lighting_snippet);
    renderer->material_lighting_snippet = NULL;

    cg_object_unref(renderer->simple_lighting_snippet);
    renderer->simple_lighting_snippet = NULL;

    cg_object_unref(renderer->shadow_mapping_fragment_snippet);
    renderer->shadow_mapping_fragment_snippet = NULL;

    cg_object_unref(renderer->pointalism_vertex_snippet);
    renderer->pointalism_vertex_snippet = NULL;

    cg_object_unref(renderer->pointalism_halo_snippet);
    renderer->pointalism_halo_snippet = NULL;

    cg_object_unref(renderer->pointalism_opaque_snippet);
    renderer->pointalism_opaque_snippet = NULL;

    cg_object_unref(renderer->cache_position_snippet);
    renderer->cache_position_snippet = NULL;

    free_shadow_map(renderer);
}

static void
add_material_for_mask(cg_pipeline_t *pipeline,
                      rig_renderer_t *renderer,
                      rig_material_t *material,
                      rig_image_source_t **sources)
{
    if (sources[SOURCE_TYPE_ALPHA_MASK]) {
        /* XXX: We assume a video source is opaque and so never add to
         * mask pipeline. */
        if (!rig_image_source_get_is_video(sources[SOURCE_TYPE_ALPHA_MASK])) {
            rig_image_source_setup_pipeline(sources[SOURCE_TYPE_ALPHA_MASK],
                                            pipeline);
            cg_pipeline_add_snippet(pipeline, renderer->alpha_mask_snippet);
        }
    }

    if (sources[SOURCE_TYPE_COLOR])
        rig_image_source_setup_pipeline(sources[SOURCE_TYPE_COLOR], pipeline);
}

static cg_pipeline_t *
get_entity_mask_pipeline(rig_renderer_t *renderer,
                         rig_entity_t *entity,
                         rut_object_t *geometry,
                         rig_material_t *material,
                         rig_image_source_t **sources,
                         get_pipeline_flags_t flags)
{
    cg_pipeline_t *pipeline;

    pipeline = get_entity_pipeline_cache(entity, CACHE_SLOT_SHADOW);

    if (pipeline) {
        if (sources[SOURCE_TYPE_COLOR] &&
            rut_object_get_type(geometry) == &rig_pointalism_grid_type) {
            int location;
            int scale, z;

            if (rig_image_source_get_is_video(sources[SOURCE_TYPE_COLOR]))
                rig_image_source_attach_frame(sources[SOURCE_TYPE_COLOR],
                                              pipeline);

            scale = rig_pointalism_grid_get_scale(geometry);
            z = rig_pointalism_grid_get_z(geometry);

            location =
                cg_pipeline_get_uniform_location(pipeline, "scale_factor");
            cg_pipeline_set_uniform_1f(pipeline, location, scale);

            location = cg_pipeline_get_uniform_location(pipeline, "z_trans");
            cg_pipeline_set_uniform_1f(pipeline, location, z);

            location = cg_pipeline_get_uniform_location(pipeline, "anti_scale");

            if (rig_pointalism_grid_get_lighter(geometry))
                cg_pipeline_set_uniform_1i(pipeline, location, 1);
            else
                cg_pipeline_set_uniform_1i(pipeline, location, 0);
        }

        if (sources[SOURCE_TYPE_ALPHA_MASK]) {
            int location;
            if (rig_image_source_get_is_video(sources[SOURCE_TYPE_ALPHA_MASK]))
                rig_image_source_attach_frame(sources[SOURCE_TYPE_COLOR],
                                              pipeline);

            location = cg_pipeline_get_uniform_location(
                pipeline, "material_alpha_threshold");
            cg_pipeline_set_uniform_1f(
                pipeline, location, material->alpha_mask_threshold);
        }

        return cg_object_ref(pipeline);
    }

    if (rut_object_get_type(geometry) == &rig_diamond_type) {
        pipeline = cg_object_ref(renderer->dof_diamond_pipeline);
        rig_diamond_apply_mask(geometry, pipeline);

        if (material)
            add_material_for_mask(pipeline, renderer, material, sources);
    } else if (rut_object_get_type(geometry) == &rig_shape_type) {
        pipeline = cg_pipeline_copy(renderer->dof_unshaped_pipeline);

        if (rig_shape_get_shaped(geometry)) {
            cg_texture_t *shape_texture = rig_shape_get_shape_texture(geometry);

            cg_pipeline_set_layer_texture(pipeline, 0, shape_texture);
        }

        if (material)
            add_material_for_mask(pipeline, renderer, material, sources);
    } else if (rut_object_get_type(geometry) == &rig_nine_slice_type) {
        pipeline = cg_pipeline_copy(renderer->dof_unshaped_pipeline);

        if (material)
            add_material_for_mask(pipeline, renderer, material, sources);
    } else if (rut_object_get_type(geometry) == &rig_pointalism_grid_type) {
        pipeline = cg_pipeline_copy(renderer->dof_diamond_pipeline);

        if (material) {
            if (sources[SOURCE_TYPE_COLOR]) {
                rig_image_source_set_first_layer(sources[SOURCE_TYPE_COLOR], 1);
                rig_image_source_set_default_sample(sources[SOURCE_TYPE_COLOR],
                                                    false);
                rig_image_source_setup_pipeline(sources[SOURCE_TYPE_COLOR],
                                                pipeline);
                cg_pipeline_add_snippet(pipeline,
                                        renderer->pointalism_vertex_snippet);
            }

            if (sources[SOURCE_TYPE_ALPHA_MASK]) {
                rig_image_source_set_first_layer(
                    sources[SOURCE_TYPE_ALPHA_MASK], 4);
                rig_image_source_set_default_sample(sources[SOURCE_TYPE_COLOR],
                                                    false);
                rig_image_source_setup_pipeline(sources[SOURCE_TYPE_COLOR],
                                                pipeline);
                cg_pipeline_add_snippet(pipeline, renderer->alpha_mask_snippet);
            }
        }
    } else
        pipeline = cg_object_ref(renderer->dof_pipeline);

    set_entity_pipeline_cache(entity, CACHE_SLOT_SHADOW, pipeline);

    return pipeline;
}

static void
get_light_modelviewprojection(const cg_matrix_t *model_transform,
                              rig_entity_t *light,
                              const cg_matrix_t *light_projection,
                              cg_matrix_t *light_mvp)
{
    const cg_matrix_t *light_transform;
    cg_matrix_t light_view;

    /* TODO: cache the bias * light_projection * light_view matrix! */

    /* Transform from NDC coords to texture coords (with 0,0)
     * top-left. (column major order) */
    float bias[16] = { .5f,  .0f, .0f, .0f,
                       .0f, -.5f, .0f, .0f,
                       .0f,  .0f, .5f, .0f,
                       .5f,  .5f, .5f, 1.f };

    light_transform = rig_entity_get_transform(light);
    cg_matrix_get_inverse(light_transform, &light_view);

    cg_matrix_init_from_array(light_mvp, bias);
    cg_matrix_multiply(light_mvp, light_mvp, light_projection);
    cg_matrix_multiply(light_mvp, light_mvp, &light_view);

    cg_matrix_multiply(light_mvp, light_mvp, model_transform);
}

static void
image_source_changed_cb(rig_image_source_t *source, void *user_data)
{
    rig_engine_t *engine = user_data;

    rut_shell_queue_redraw(engine->shell);
}

static cg_pipeline_t *
get_entity_color_pipeline(rig_renderer_t *renderer,
                          rig_entity_t *entity,
                          rut_object_t *geometry,
                          rig_material_t *material,
                          rig_image_source_t **sources,
                          get_pipeline_flags_t flags,
                          bool blended)
{
    rig_engine_t *engine = renderer->engine;
    rig_renderer_priv_t *priv = entity->renderer_priv;
    cg_depth_state_t depth_state;
    cg_pipeline_t *pipeline;
    cg_pipeline_t *fin_pipeline;
    cg_framebuffer_t *shadow_fb;
    cg_snippet_t *blend = renderer->blended_discard_snippet;
    cg_snippet_t *unblend = renderer->unblended_discard_snippet;
    rut_object_t *hair;
    int i;

    /* TODO: come up with a scheme for minimizing how many separate
     * cg_pipeline_ts we create or at least deriving the pipelines
     * from a small set of templates.
     */

    hair = rig_entity_get_component(entity, RUT_COMPONENT_TYPE_HAIR);

    if (blended) {
        pipeline = get_entity_pipeline_cache(entity, CACHE_SLOT_COLOR_BLENDED);
        if (hair)
            fin_pipeline =
                get_entity_pipeline_cache(entity, CACHE_SLOT_HAIR_FINS_BLENDED);
    } else {
        pipeline =
            get_entity_pipeline_cache(entity, CACHE_SLOT_COLOR_UNBLENDED);
        if (hair)
            fin_pipeline = get_entity_pipeline_cache(
                entity, CACHE_SLOT_HAIR_FINS_UNBLENDED);
    }

    if (pipeline) {
        cg_object_ref(pipeline);
        goto FOUND;
    }

    pipeline = cg_pipeline_new(engine->shell->cg_device);

    if (sources[SOURCE_TYPE_COLOR])
        rig_image_source_setup_pipeline(sources[SOURCE_TYPE_COLOR], pipeline);
    if (sources[SOURCE_TYPE_ALPHA_MASK])
        rig_image_source_setup_pipeline(sources[SOURCE_TYPE_ALPHA_MASK],
                                        pipeline);
    if (sources[SOURCE_TYPE_NORMAL_MAP])
        rig_image_source_setup_pipeline(sources[SOURCE_TYPE_NORMAL_MAP],
                                        pipeline);

    cg_pipeline_set_color4f(pipeline, 0.8f, 0.8f, 0.8f, 1.f);

    /* enable depth testing */
    cg_depth_state_init(&depth_state);
    cg_depth_state_set_test_enabled(&depth_state, true);

    if (blended)
        cg_depth_state_set_write_enabled(&depth_state, false);

    cg_pipeline_set_depth_state(pipeline, &depth_state, NULL);

    cg_pipeline_add_snippet(pipeline, renderer->cache_position_snippet);

    /* vertex_t shader setup for lighting */

    cg_pipeline_add_snippet(pipeline, renderer->lighting_vertex_snippet);

    if (sources[SOURCE_TYPE_NORMAL_MAP])
        cg_pipeline_add_snippet(pipeline, renderer->normal_map_vertex_snippet);

    if (rig_material_get_receive_shadow(material))
        cg_pipeline_add_snippet(pipeline,
                                renderer->shadow_mapping_vertex_snippet);

    if (rut_object_get_type(geometry) == &rig_nine_slice_type &&
        !priv->nine_slice_closure) {
        priv->nine_slice_closure = rig_nine_slice_add_update_callback(
            (rig_nine_slice_t *)geometry, nine_slice_changed_cb, NULL, NULL);
    } else if (rut_object_get_type(geometry) == &rig_shape_type) {
        cg_texture_t *shape_texture;

        if (rig_shape_get_shaped(geometry)) {
            shape_texture = rig_shape_get_shape_texture(geometry);
            cg_pipeline_set_layer_texture(pipeline, 0, shape_texture);
        }

        if (priv->reshaped_closure) {
            priv->reshaped_closure = rig_shape_add_reshaped_callback(
                geometry, reshape_cb, NULL, NULL);
        }
    } else if (rut_object_get_type(geometry) == &rig_diamond_type &&
               !priv->diamond_closure) {
        rig_diamond_apply_mask(geometry, pipeline);

        priv->diamond_closure = rig_diamond_add_update_callback(
            (rig_diamond_t *)geometry, diamond_changed_cb, NULL, NULL);
    } else if (rut_object_get_type(geometry) == &rig_pointalism_grid_type &&
               sources[SOURCE_TYPE_COLOR]) {
        if (!priv->pointalism_grid_update_closure) {
            rig_pointalism_grid_t *grid = (rig_pointalism_grid_t *)geometry;
            priv->pointalism_grid_update_closure =
                rig_pointalism_grid_add_update_callback(
                    grid, pointalism_changed_cb, NULL, NULL);
        }

        cg_pipeline_set_layer_texture(pipeline, 0, engine->shell->circle_texture);
        cg_pipeline_set_layer_filters(pipeline,
                                      0,
                                      CG_PIPELINE_FILTER_LINEAR_MIPMAP_LINEAR,
                                      CG_PIPELINE_FILTER_LINEAR);

        cg_pipeline_add_snippet(pipeline, renderer->pointalism_vertex_snippet);

        blend = renderer->pointalism_halo_snippet;
        unblend = renderer->pointalism_opaque_snippet;
    }

    /*if (hair)
       {
        cg_pipeline_add_snippet (pipeline, renderer->hair_fin_snippet);
        rig_hair_set_uniform_location (hair, pipeline,
                                       blended ? RIG_HAIR_SHELL_POSITION_BLENDED
       :
                                       RIG_HAIR_SHELL_POSITION_UNBLENDED);
       }*/

    /* and fragment shader */

    /* XXX: ideally we wouldn't have to rely on conditionals + discards
     * in the fragment shader to differentiate blended and unblended
     * regions and instead we should let users mark out opaque regions
     * in geometry.
     */
    cg_pipeline_add_snippet(pipeline, blended ? blend : unblend);

    cg_pipeline_add_snippet(pipeline, renderer->unpremultiply_snippet);

    if (hair) {
        if (sources[SOURCE_TYPE_COLOR] || sources[SOURCE_TYPE_ALPHA_MASK] ||
            sources[SOURCE_TYPE_NORMAL_MAP]) {
            cg_pipeline_add_snippet(pipeline, renderer->hair_material_snippet);
        } else
            cg_pipeline_add_snippet(pipeline, renderer->hair_simple_snippet);

        cg_pipeline_set_layer_combine(
            pipeline, 11, "RGBA=REPLACE(PREVIOUS)", NULL);

        fin_pipeline = cg_pipeline_copy(pipeline);
        cg_pipeline_add_snippet(fin_pipeline, renderer->hair_fin_snippet);
        cg_pipeline_add_snippet(pipeline, renderer->hair_vertex_snippet);
        rig_hair_set_uniform_location(hair,
                                      pipeline,
                                      blended
                                      ? RIG_HAIR_SHELL_POSITION_BLENDED
                                      : RIG_HAIR_SHELL_POSITION_UNBLENDED);
        rig_hair_set_uniform_location(hair, fin_pipeline, RIG_HAIR_LENGTH);
    } else if (sources[SOURCE_TYPE_COLOR] || sources[SOURCE_TYPE_ALPHA_MASK] ||
               sources[SOURCE_TYPE_NORMAL_MAP]) {
        if (sources[SOURCE_TYPE_ALPHA_MASK])
            cg_pipeline_add_snippet(pipeline, renderer->alpha_mask_snippet);

        if (sources[SOURCE_TYPE_NORMAL_MAP])
            cg_pipeline_add_snippet(pipeline,
                                    renderer->normal_map_fragment_snippet);
        else
            cg_pipeline_add_snippet(pipeline,
                                    renderer->material_lighting_snippet);
    } else
        cg_pipeline_add_snippet(pipeline, renderer->simple_lighting_snippet);

    if (rig_material_get_receive_shadow(material)) {
        /* Hook the shadow map sampling */

        cg_pipeline_set_layer_texture(pipeline, 10, renderer->shadow_map);
        /* For debugging the shadow mapping... */
        // cg_pipeline_set_layer_texture (pipeline, 7, renderer->shadow_color);
        // cg_pipeline_set_layer_texture (pipeline, 7, renderer->gradient);

        /* We don't want this layer to be automatically modulated with the
         * previous layers so we set its combine mode to "REPLACE" so it
         * will be skipped past and we can sample its texture manually */
        cg_pipeline_set_layer_combine(
            pipeline, 10, "RGBA=REPLACE(PREVIOUS)", NULL);

        /* Handle shadow mapping */
        cg_pipeline_add_snippet(pipeline,
                                renderer->shadow_mapping_fragment_snippet);
    }

    cg_pipeline_add_snippet(pipeline, renderer->premultiply_snippet);

    if (hair)
        cg_pipeline_add_snippet(fin_pipeline, renderer->premultiply_snippet);

    if (!blended) {
        cg_pipeline_set_blend(pipeline, "RGBA = ADD (SRC_COLOR, 0)", NULL);
        set_entity_pipeline_cache(entity, CACHE_SLOT_COLOR_UNBLENDED, pipeline);
        if (hair) {
            cg_pipeline_set_blend(
                fin_pipeline, "RGBA = ADD (SRC_COLOR, 0)", NULL);
            set_entity_pipeline_cache(
                entity, CACHE_SLOT_HAIR_FINS_UNBLENDED, fin_pipeline);
        }
    } else {
        set_entity_pipeline_cache(entity, CACHE_SLOT_COLOR_BLENDED, pipeline);
        if (hair)
            set_entity_pipeline_cache(
                entity, CACHE_SLOT_HAIR_FINS_BLENDED, fin_pipeline);
    }

FOUND:

    /* FIXME: there's lots to optimize about this! */
    shadow_fb = renderer->shadow_fb;

    /* update uniforms in pipelines */
    {
        cg_matrix_t light_shadow_matrix, light_projection;
        cg_matrix_t model_transform;
        const float *light_matrix;
        int location;

        cg_framebuffer_get_projection_matrix(shadow_fb, &light_projection);

        /* TODO: use Cogl's MatrixStack api so we can update the entity
         * model matrix incrementally as we traverse the scenegraph */
        rut_graphable_get_transform(entity, &model_transform);

        get_light_modelviewprojection(&model_transform,
                                      engine->ui->light,
                                      &light_projection,
                                      &light_shadow_matrix);

        light_matrix = cg_matrix_get_array(&light_shadow_matrix);

        location =
            cg_pipeline_get_uniform_location(pipeline, "light_shadow_matrix");
        cg_pipeline_set_uniform_matrix(
            pipeline, location, 4, 1, false, light_matrix);
        if (hair)
            cg_pipeline_set_uniform_matrix(
                fin_pipeline, location, 4, 1, false, light_matrix);

        for (i = 0; i < 3; i++)
            if (sources[i])
                rig_image_source_attach_frame(sources[i], pipeline);
    }

    return pipeline;
}

static void
image_source_ready_cb(rig_image_source_t *source, void *user_data)
{
    rig_entity_t *entity = user_data;
    rig_image_source_t *color_src;
    rut_object_t *geometry;
    rig_material_t *material;
    float width, height;

    geometry = rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);
    material = rig_entity_get_component(entity, RUT_COMPONENT_TYPE_MATERIAL);

    dirty_entity_pipelines(entity);

    if (material->color_source_asset)
        color_src = get_entity_image_source_cache(entity, SOURCE_TYPE_COLOR);
    else
        color_src = NULL;

    /* If the color source has changed then we may also need to update
     * the geometry according to the size of the color source */
    if (source != color_src)
        return;

    rig_image_source_get_natural_size(source, &width, &height);

    if (rut_object_is(geometry, RUT_TRAIT_ID_IMAGE_SIZE_DEPENDENT)) {
        rut_image_size_dependant_vtable_t *dependant =
            rut_object_get_vtable(geometry, RUT_TRAIT_ID_IMAGE_SIZE_DEPENDENT);
        dependant->set_image_size(geometry, width, height);
    }
}

static cg_pipeline_t *
get_entity_pipeline(rig_renderer_t *renderer,
                    rig_entity_t *entity,
                    rut_component_t *geometry,
                    rig_pass_t pass)
{
    rig_engine_t *engine = renderer->engine;
    rig_material_t *material =
        rig_entity_get_component(entity, RUT_COMPONENT_TYPE_MATERIAL);
    rig_image_source_t *sources[3];
    get_pipeline_flags_t flags = 0;
    rig_asset_t *asset;

    c_return_val_if_fail(material != NULL, NULL);

    /* FIXME: Instead of having rig_entity apis for caching image
     * sources, we should allow the renderer to track arbitrary
     * private state with entities so it can better manage caches
     * of different kinds of derived, renderer specific state.
     */

    sources[SOURCE_TYPE_COLOR] =
        get_entity_image_source_cache(entity, SOURCE_TYPE_COLOR);
    sources[SOURCE_TYPE_ALPHA_MASK] =
        get_entity_image_source_cache(entity, SOURCE_TYPE_ALPHA_MASK);
    sources[SOURCE_TYPE_NORMAL_MAP] =
        get_entity_image_source_cache(entity, SOURCE_TYPE_NORMAL_MAP);

    /* Materials may be associated with various image sources which need
     * to be setup before we try creating pipelines and querying the
     * geometry of entities because many components are influenced by
     * the size of associated images being mapped.
     */
    asset = material->color_source_asset;

    if (asset && !sources[SOURCE_TYPE_COLOR]) {
        sources[SOURCE_TYPE_COLOR] = rig_asset_get_image_source(asset);

        set_entity_image_source_cache(
            entity, SOURCE_TYPE_COLOR, sources[SOURCE_TYPE_COLOR]);
#warning "FIXME: we need to track this as renderer priv since we're leaking closures a.t.m"
        rig_image_source_add_ready_callback(
            sources[SOURCE_TYPE_COLOR], image_source_ready_cb, entity, NULL);
        rig_image_source_add_on_changed_callback(
            sources[SOURCE_TYPE_COLOR], image_source_changed_cb, engine, NULL);

        rig_image_source_set_first_layer(sources[SOURCE_TYPE_COLOR], 1);
    }

    asset = material->alpha_mask_asset;

    if (asset && !sources[SOURCE_TYPE_ALPHA_MASK]) {
        sources[SOURCE_TYPE_ALPHA_MASK] = rig_asset_get_image_source(asset);

        set_entity_image_source_cache(
            entity, 1, sources[SOURCE_TYPE_ALPHA_MASK]);
#warning "FIXME: we need to track this as renderer priv since we're leaking closures a.t.m"
        rig_image_source_add_ready_callback(sources[SOURCE_TYPE_ALPHA_MASK],
                                            image_source_ready_cb,
                                            entity,
                                            NULL);
        rig_image_source_add_on_changed_callback(
            sources[SOURCE_TYPE_ALPHA_MASK],
            image_source_changed_cb,
            engine,
            NULL);

        rig_image_source_set_first_layer(sources[SOURCE_TYPE_ALPHA_MASK], 4);
        rig_image_source_set_default_sample(sources[SOURCE_TYPE_ALPHA_MASK],
                                            false);
    }

    asset = material->normal_map_asset;

    if (asset && !sources[SOURCE_TYPE_NORMAL_MAP]) {
        sources[SOURCE_TYPE_NORMAL_MAP] =
            rig_asset_get_image_source(asset);

        set_entity_image_source_cache(
            entity, 2, sources[SOURCE_TYPE_NORMAL_MAP]);
#warning "FIXME: we need to track this as renderer priv since we're leaking closures a.t.m"
        rig_image_source_add_ready_callback(sources[SOURCE_TYPE_NORMAL_MAP],
                                            image_source_ready_cb,
                                            entity,
                                            NULL);
        rig_image_source_add_on_changed_callback(
            sources[SOURCE_TYPE_NORMAL_MAP],
            image_source_changed_cb,
            engine,
            NULL);

        rig_image_source_set_first_layer(sources[SOURCE_TYPE_NORMAL_MAP], 7);
        rig_image_source_set_default_sample(sources[SOURCE_TYPE_NORMAL_MAP],
                                            false);
    }

    if (pass == RIG_PASS_COLOR_UNBLENDED)
        return get_entity_color_pipeline(renderer, entity, geometry, material,
                                         sources, flags, false);
    else if (pass == RIG_PASS_COLOR_BLENDED)
        return get_entity_color_pipeline(renderer, entity, geometry, material,
                                         sources, flags, true);
    else if (pass == RIG_PASS_DOF_DEPTH || pass == RIG_PASS_SHADOW)
        return get_entity_mask_pipeline(renderer, entity, geometry, material,
                                        sources, flags);

    c_warn_if_reached();
    return NULL;
}
static void
get_normal_matrix(const cg_matrix_t *matrix, float *normal_matrix)
{
    cg_matrix_t inverse_matrix;

    /* Invert the matrix */
    cg_matrix_get_inverse(matrix, &inverse_matrix);

    /* Transpose it while converting it to 3x3 */
    normal_matrix[0] = inverse_matrix.xx;
    normal_matrix[1] = inverse_matrix.xy;
    normal_matrix[2] = inverse_matrix.xz;

    normal_matrix[3] = inverse_matrix.yx;
    normal_matrix[4] = inverse_matrix.yy;
    normal_matrix[5] = inverse_matrix.yz;

    normal_matrix[6] = inverse_matrix.zx;
    normal_matrix[7] = inverse_matrix.zy;
    normal_matrix[8] = inverse_matrix.zz;
}

static void
ensure_renderer_priv(rig_entity_t *entity, rig_renderer_t *renderer)
{
    /* If this entity was last renderered with some other renderer then
     * we free any private state associated with the previous renderer
     * before creating our own private state */
    if (entity->renderer_priv) {
        rut_object_t *renderer = *(rut_object_t **)entity->renderer_priv;
        if (rut_object_get_type(renderer) != &rig_renderer_type)
            rut_renderer_free_priv(renderer, entity);
    }

    if (!entity->renderer_priv) {
        rig_renderer_priv_t *priv = c_slice_new0(rig_renderer_priv_t);

        priv->renderer = renderer;
        entity->renderer_priv = priv;
    }
}

static void
rig_renderer_flush_journal(rig_renderer_t *renderer,
                           rig_paint_context_t *paint_ctx)
{
    c_array_t *journal = renderer->journal;
    rut_paint_context_t *rut_paint_ctx = &paint_ctx->_parent;
    rut_object_t *camera = rut_paint_ctx->camera;
    cg_framebuffer_t *fb = rut_camera_get_framebuffer(camera);
    int start, dir, end;
    int i;

    /* TODO: use an inline qsort implementation */
    c_array_sort(journal, (void *)sort_entry_cb);

    /* We draw opaque geometry front-to-back so we are more likely to be
     * able to discard later fragments earlier by depth testing.
     *
     * We draw transparent geometry back-to-front so it blends
     * correctly.
     */
    if (paint_ctx->pass == RIG_PASS_COLOR_BLENDED) {
        start = 0;
        dir = 1;
        end = journal->len;
    } else {
        start = journal->len - 1;
        dir = -1;
        end = -1;
    }

    cg_framebuffer_push_matrix(fb);

    for (i = start; i != end; i += dir) {
        rig_journal_entry_t *entry =
            &c_array_index(journal, rig_journal_entry_t, i);
        rig_entity_t *entity = entry->entity;
        rut_object_t *geometry =
            rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);
        cg_pipeline_t *pipeline;
        cg_primitive_t *primitive;
        float normal_matrix[9];
        rig_material_t *material;
        cg_pipeline_t *fin_pipeline = NULL;
        rig_hair_t *hair;

        if (rut_object_get_type(geometry) == &rut_text_type &&
            paint_ctx->pass == RIG_PASS_COLOR_BLENDED) {
            cg_framebuffer_set_modelview_matrix(fb, &entry->matrix);
            rut_paintable_paint(geometry, rut_paint_ctx);
            continue;
        }

        if (!rut_object_is(geometry, RUT_TRAIT_ID_PRIMABLE))
            continue;

        /*
         * Setup Pipelines...
         */

        pipeline =
            get_entity_pipeline(renderer, entity, geometry, paint_ctx->pass);

        material =
            rig_entity_get_component(entity, RUT_COMPONENT_TYPE_MATERIAL);

        hair = rig_entity_get_component(entity, RUT_COMPONENT_TYPE_HAIR);
        if (hair) {
            rig_hair_update_state(hair);

            if (rut_object_get_type(geometry) == &rig_model_type) {
                if (paint_ctx->pass == RIG_PASS_COLOR_BLENDED) {
                    fin_pipeline = get_entity_pipeline_cache(
                        entity, CACHE_SLOT_HAIR_FINS_BLENDED);
                } else {
                    fin_pipeline = get_entity_pipeline_cache(
                        entity, CACHE_SLOT_HAIR_FINS_UNBLENDED);
                }
            }
        }

        /*
         * Update Uniforms...
         */

        if ((paint_ctx->pass == RIG_PASS_DOF_DEPTH ||
             paint_ctx->pass == RIG_PASS_SHADOW)) {
            /* FIXME: avoid updating these uniforms for every primitive if
             * the focal parameters haven't change! */
            set_focal_parameters(pipeline,
                                 rut_camera_get_focal_distance(camera),
                                 rut_camera_get_depth_of_field(camera));

        } else if ((paint_ctx->pass == RIG_PASS_COLOR_UNBLENDED ||
                    paint_ctx->pass == RIG_PASS_COLOR_BLENDED)) {
            rig_ui_t *ui = engine->ui;
            rig_light_t *light =
                rig_entity_get_component(ui->light, RUT_COMPONENT_TYPE_LIGHT);
            int location;

            /* FIXME: only update the lighting uniforms when the light has
             * actually moved! */
            rig_light_set_uniforms(light, pipeline);

            /* FIXME: only update the material uniforms when the material has
             * actually changed! */
            if (material)
                rig_material_flush_uniforms(material, pipeline);

            get_normal_matrix(&entry->matrix, normal_matrix);

            location =
                cg_pipeline_get_uniform_location(pipeline, "normal_matrix");
            cg_pipeline_set_uniform_matrix(pipeline,
                                           location,
                                           3, /* dimensions */
                                           1, /* count */
                                           false, /* don't transpose again */
                                           normal_matrix);

            if (fin_pipeline) {
                rig_light_set_uniforms(light, fin_pipeline);

                if (material)
                    rig_material_flush_uniforms(material, fin_pipeline);

                cg_pipeline_set_uniform_matrix(
                    fin_pipeline,
                    location,
                    3, /* dimensions */
                    1, /* count */
                    false, /* don't transpose again */
                    normal_matrix);

                cg_pipeline_set_layer_texture(
                    fin_pipeline, 11, hair->fin_texture);

                rig_hair_set_uniform_float_value(
                    hair, fin_pipeline, RIG_HAIR_LENGTH, hair->length);
            }
        }

        /*
         * Draw Primitive...
         */

        primitive = get_entity_primitive_cache(entity, 0);
        if (!primitive) {
            primitive = rut_primable_get_primitive(geometry);
            set_entity_primitive_cache(entity, 0, primitive);
        }

        cg_framebuffer_set_modelview_matrix(fb, &entry->matrix);

        if (hair) {
            cg_texture_t *texture;
            int i, uniform;

            if (fin_pipeline) {
                rig_model_t *model = geometry;
                cg_primitive_draw(model->fin_primitive, fb, fin_pipeline);
            }

            if (paint_ctx->pass == RIG_PASS_COLOR_BLENDED)
                uniform = RIG_HAIR_SHELL_POSITION_BLENDED;
            else if (paint_ctx->pass == RIG_PASS_COLOR_UNBLENDED)
                uniform = RIG_HAIR_SHELL_POSITION_UNBLENDED;
            else if (paint_ctx->pass == RIG_PASS_DOF_DEPTH ||
                     paint_ctx->pass == RIG_PASS_SHADOW)
                uniform = RIG_HAIR_SHELL_POSITION_SHADOW;
            else {
                c_warn_if_reached();
                uniform = RIG_HAIR_SHELL_POSITION_BLENDED;
            }

            /* FIXME: only update the hair uniforms when they change! */
            /* FIXME: avoid needing to query the uniform locations by
             * name for each primitive! */

            texture = c_array_index(hair->shell_textures, cg_texture_t *, 0);

            cg_pipeline_set_layer_texture(pipeline, 11, texture);

            rig_hair_set_uniform_float_value(hair, pipeline, uniform, 0);

            /* TODO: we should be drawing the original base model as
             * the interior, with depth write and test enabled to
             * make sure we reduce the work involved in blending all
             * the shells on top. */
            cg_primitive_draw(primitive, fb, pipeline);

            cg_pipeline_set_alpha_test_function(
                pipeline, CG_PIPELINE_ALPHA_FUNC_GREATER, 0.49);

            /* TODO: we should support having more shells than
             * real textures... */
            for (i = 1; i < hair->n_shells; i++) {
                float hair_pos = hair->shell_positions[i];

                texture =
                    c_array_index(hair->shell_textures, cg_texture_t *, i);
                cg_pipeline_set_layer_texture(pipeline, 11, texture);

                rig_hair_set_uniform_float_value(
                    hair, pipeline, uniform, hair_pos);

                cg_primitive_draw(primitive, fb, pipeline);
            }
        } else
            cg_primitive_draw(primitive, fb, pipeline);

        cg_object_unref(pipeline);

        rut_object_unref(entry->entity);
    }

    cg_framebuffer_pop_matrix(fb);

    c_array_set_size(journal, 0);
}

static void
draw_entity_camera_frustum(rig_engine_t *engine,
                           rig_entity_t *entity,
                           cg_framebuffer_t *fb)
{
    rut_object_t *camera =
        rig_entity_get_component(entity, RUT_COMPONENT_TYPE_CAMERA);
    cg_primitive_t *primitive = rut_camera_create_frustum_primitive(camera);
    cg_pipeline_t *pipeline = cg_pipeline_new(engine->shell->cg_device);
    cg_depth_state_t depth_state;

    /* enable depth testing */
    cg_depth_state_init(&depth_state);
    cg_depth_state_set_test_enabled(&depth_state, true);
    cg_pipeline_set_depth_state(pipeline, &depth_state, NULL);

    rut_util_draw_jittered_primitive3f(fb, primitive, 0.8, 0.6, 0.1);

    cg_object_unref(primitive);
    cg_object_unref(pipeline);
}

static void
text_preferred_size_changed_cb(rut_object_t *sizable,
                               void *user_data)
{
    rut_text_t *text = sizable;
    float width, height;
    rut_property_t *width_prop = &text->properties[RUT_TEXT_PROP_WIDTH];

    if (width_prop->binding)
        width = rut_property_get_float(width_prop);
    else {
        rut_sizable_get_preferred_width(text, -1, NULL, &width);
    }

    rut_sizable_get_preferred_height(text, width, NULL, &height);
    rut_sizable_set_size(text, width, height);
}

static rut_traverse_visit_flags_t
entitygraph_pre_paint_cb(rut_object_t *object, int depth, void *user_data)
{
    rig_paint_context_t *paint_ctx = user_data;
    rut_paint_context_t *rut_paint_ctx = user_data;
    rig_renderer_t *renderer = paint_ctx->renderer;
    rut_object_t *camera = rut_paint_ctx->camera;
    cg_framebuffer_t *fb = rut_camera_get_framebuffer(camera);

    if (rut_object_is(object, RUT_TRAIT_ID_TRANSFORMABLE)) {
        const cg_matrix_t *matrix = rut_transformable_get_matrix(object);
        cg_framebuffer_push_matrix(fb);
        cg_framebuffer_transform(fb, matrix);
    }

    if (rut_object_get_type(object) == &rig_entity_type) {
        rig_entity_t *entity = object;
        rig_material_t *material;
        rut_object_t *geometry;
        cg_matrix_t matrix;
        rig_renderer_priv_t *priv;

        material =
            rig_entity_get_component(entity, RUT_COMPONENT_TYPE_MATERIAL);
        if (!material || !rig_material_get_visible(material))
            return RUT_TRAVERSE_VISIT_CONTINUE;

        if (paint_ctx->pass == RIG_PASS_SHADOW &&
            !rig_material_get_cast_shadow(material))
            return RUT_TRAVERSE_VISIT_CONTINUE;

        geometry = rig_entity_get_component(object, RUT_COMPONENT_TYPE_GEOMETRY);
        if (!geometry)
            return RUT_TRAVERSE_VISIT_CONTINUE;

        ensure_renderer_priv(entity, renderer);
        priv = entity->renderer_priv;

        /* XXX: Ideally the renderer code wouldn't have to handle this
         * but for now we make sure to allocate all text components
         * their preferred size before rendering them.
         *
         * Note: we first check to see if the text component has a
         * binding for the width property, and if so we assume the
         * UI is constraining the width and wants the text to be
         * wrapped.
         */
        if (rut_object_get_type(geometry) == &rut_text_type) {
            rut_text_t *text = geometry;

            if (!priv->preferred_size_closure) {
                priv->preferred_size_closure =
                    rut_sizable_add_preferred_size_callback(
                        text,
                        text_preferred_size_changed_cb,
                        NULL, /* user data */
                        NULL); /* destroy */
                text_preferred_size_changed_cb(text, NULL);
            }
        }

        cg_framebuffer_get_modelview_matrix(fb, &matrix);
        rig_journal_log(renderer->journal, paint_ctx, entity, &matrix);

        return RUT_TRAVERSE_VISIT_CONTINUE;
    }

    return RUT_TRAVERSE_VISIT_CONTINUE;
}

static rut_traverse_visit_flags_t
entitygraph_post_paint_cb(rut_object_t *object, int depth, void *user_data)
{
    if (rut_object_is(object, RUT_TRAIT_ID_TRANSFORMABLE)) {
        rut_paint_context_t *rut_paint_ctx = user_data;
        cg_framebuffer_t *fb =
            rut_camera_get_framebuffer(rut_paint_ctx->camera);
        cg_framebuffer_pop_matrix(fb);
    }

    return RUT_TRAVERSE_VISIT_CONTINUE;
}

void
paint_camera_entity_pass(rig_paint_context_t *paint_ctx,
                         rig_entity_t *camera_entity)
{
    rut_paint_context_t *rut_paint_ctx = &paint_ctx->_parent;
    rut_object_t *saved_camera = rut_paint_ctx->camera;
    rut_object_t *camera =
        rig_entity_get_component(camera_entity, RUT_COMPONENT_TYPE_CAMERA);
    cg_framebuffer_t *fb = rut_camera_get_framebuffer(camera);
    rig_renderer_t *renderer = paint_ctx->renderer;
    rig_engine_t *engine = paint_ctx->engine;
    cg_device_t *dev = engine->shell->cg_device;
    rig_ui_t *ui = engine->ui;

    rut_paint_ctx->camera = camera;

    rut_camera_flush(camera);

    /* Note: if we are rendering with the real ui->play_camera (i.e.
     * not a viewing camera then we don't clear the background with
     * a rectangle like this, we can just clear the framebuffer...
     */
    if (paint_ctx->pass == RIG_PASS_COLOR_UNBLENDED &&
        camera != ui->play_camera)
    {
        cg_pipeline_t *pipeline = cg_pipeline_new(dev);
        const cg_color_t *bg_color =
            rut_camera_get_background_color(ui->play_camera_component);
        cg_pipeline_set_color4f(pipeline,
                                bg_color->red,
                                bg_color->green,
                                bg_color->blue,
                                bg_color->alpha);
        cg_framebuffer_draw_rectangle(fb, pipeline,
                                      0, 0,
                                      engine->device_width,
                                      engine->device_height);
        cg_object_unref(pipeline);
    }

    rut_graphable_traverse(engine->ui->scene,
                           RUT_TRAVERSE_DEPTH_FIRST,
                           entitygraph_pre_paint_cb,
                           entitygraph_post_paint_cb,
                           paint_ctx);

    rig_renderer_flush_journal(renderer, paint_ctx);

    rut_camera_end_frame(camera);

    rut_paint_ctx->camera = saved_camera;
}

void
rig_renderer_paint_camera(rig_paint_context_t *paint_ctx,
                          rig_entity_t *camera_entity)
{
    rut_object_t *camera =
        rig_entity_get_component(camera_entity, RUT_COMPONENT_TYPE_CAMERA);
    cg_framebuffer_t *fb = rut_camera_get_framebuffer(camera);
    rig_renderer_t *renderer = paint_ctx->renderer;
    rig_engine_t *engine = paint_ctx->engine;
    rig_ui_t *ui = engine->ui;

    paint_ctx->pass = RIG_PASS_SHADOW;
    rig_entity_set_camera_view_from_transform(ui->light);
    paint_camera_entity_pass(paint_ctx, ui->light);

    if (paint_ctx->enable_dof) {
        const float *viewport = rut_camera_get_viewport(camera);
        int width = viewport[2];
        int height = viewport[3];
        int save_viewport_x = viewport[0];
        int save_viewport_y = viewport[1];
        cg_framebuffer_t *pass_fb;
        const cg_color_t *bg_color;
        rig_depth_of_field_t *dof = renderer->dof;

        if (!dof)
            renderer->dof = dof = rig_dof_effect_new(engine);

        rig_dof_effect_set_framebuffer_size(dof, width, height);

        pass_fb = rig_dof_effect_get_depth_pass_fb(dof);
        rut_camera_set_framebuffer(camera, pass_fb);
        rut_camera_set_viewport(camera, 0, 0, width, height);

        rut_camera_flush(camera);
        cg_framebuffer_clear4f(pass_fb,
                               CG_BUFFER_BIT_COLOR | CG_BUFFER_BIT_DEPTH,
                               1, 1, 1, 1);
        rut_camera_end_frame(camera);

        paint_ctx->pass = RIG_PASS_DOF_DEPTH;
        paint_camera_entity_pass(paint_ctx, camera_entity);

        pass_fb = rig_dof_effect_get_color_pass_fb(dof);
        rut_camera_set_framebuffer(camera, pass_fb);

        rut_camera_flush(camera);
        bg_color = rut_camera_get_background_color(camera);
        cg_framebuffer_clear4f(pass_fb,
                               CG_BUFFER_BIT_COLOR | CG_BUFFER_BIT_DEPTH,
                               bg_color->red,
                               bg_color->green,
                               bg_color->blue,
                               bg_color->alpha);
        rut_camera_end_frame(camera);

        paint_ctx->pass = RIG_PASS_COLOR_UNBLENDED;
        paint_camera_entity_pass(paint_ctx, camera_entity);

        paint_ctx->pass = RIG_PASS_COLOR_BLENDED;
        paint_camera_entity_pass(paint_ctx, camera_entity);

        rut_camera_set_framebuffer(camera, fb);
        rut_camera_set_viewport(camera,
                                save_viewport_x, save_viewport_y,
                                width, height);

        rut_camera_set_framebuffer(renderer->composite_camera, fb);
        rut_camera_set_viewport(renderer->composite_camera,
                                save_viewport_x, save_viewport_y,
                                width, height);

        rut_camera_flush(renderer->composite_camera);
        rig_dof_effect_draw_rectangle(dof, fb, 0, 0, 1, 1);
        rut_camera_end_frame(renderer->composite_camera);

    } else {
        paint_ctx->pass = RIG_PASS_COLOR_UNBLENDED;
        paint_camera_entity_pass(paint_ctx, camera_entity);

        paint_ctx->pass = RIG_PASS_COLOR_BLENDED;
        paint_camera_entity_pass(paint_ctx, camera_entity);
    }
}

/* TODO: remove this; it's just a stop-gap for rig-ui.c to be able
 * to setup the viewport for the light camera... */
cg_framebuffer_t *
rig_renderer_get_shadow_fb(rig_renderer_t *renderer)
{
    return renderer->shadow_fb;
}
