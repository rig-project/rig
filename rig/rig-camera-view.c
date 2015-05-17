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

#include <config.h>

#include <math.h>

#ifdef ENABLE_OCULUS_RIFT
#include "OVR_CAPI.h"
#endif

#include <rut.h>

#include "rig-engine.h"
#include "rig-view.h"
#include "rig-renderer.h"
#include "rig-dof-effect.h"

#include "components/rig-camera.h"
#include "components/rig-material.h"

typedef void (*entity_translate_callback_t)(rig_entity_t *entity,
                                            float start[3],
                                            float rel[3],
                                            void *user_data);

typedef void (*entity_translate_done_callback_t)(rig_entity_t *entity,
                                                 bool moved,
                                                 float start[3],
                                                 float rel[3],
                                                 void *user_data);

static void
_rig_camera_view_free(void *object)
{
    rig_camera_view_t *view = object;

    rig_camera_view_set_ui(view, NULL);

    rut_shell_remove_pre_paint_callback_by_graphable(view->shell,
                                                     view);

    rut_graphable_destroy(view);

    rut_object_unref(view->view_camera);
    rut_object_unref(view->view_camera_component);

    rut_object_free(rig_camera_view_t, view);
}

static void
init_camera_from_camera(rig_entity_t *dst_camera, rig_entity_t *src_camera)
{
    rut_object_t *dst_camera_comp =
        rig_entity_get_component(dst_camera, RUT_COMPONENT_TYPE_CAMERA);
    rut_object_t *src_camera_comp =
        rig_entity_get_component(src_camera, RUT_COMPONENT_TYPE_CAMERA);

    rut_projection_t mode = rut_camera_get_projection_mode(src_camera_comp);


    rut_camera_set_projection_mode(dst_camera_comp, mode);
    if (mode == RUT_PROJECTION_PERSPECTIVE) {
        rut_camera_set_field_of_view(dst_camera_comp,
                                     rut_camera_get_field_of_view(src_camera_comp));
        rut_camera_set_near_plane(dst_camera_comp,
                                  rut_camera_get_near_plane(src_camera_comp));
        rut_camera_set_far_plane(dst_camera_comp,
                                 rut_camera_get_far_plane(src_camera_comp));
    } else {
        float x1, y1, x2, y2;

        rut_camera_get_orthographic_coordinates(src_camera_comp,
                                                &x1, &y1, &x2, &y2);
        rut_camera_set_orthographic_coordinates(dst_camera_comp,
                                                x1, y1, x2, y2);
    }

    rut_camera_set_zoom(dst_camera_comp,
                        rut_camera_get_zoom(src_camera_comp));

    rig_entity_set_position(dst_camera, rig_entity_get_position(src_camera));
    rig_entity_set_scale(dst_camera, rig_entity_get_scale(src_camera));
    rig_entity_set_rotation(dst_camera, rig_entity_get_rotation(src_camera));
}

#ifdef ENABLE_OCULUS_RIFT
static void
paint_eye(rig_camera_view_t *view,
          rig_paint_context_t *rig_paint_ctx,
          rig_entity_t *camera,
          rig_camera_t *camera_component,
          struct eye *eye)
{
    cg_quaternion_t orientation;

    rut_graphable_add_child(view->ui->scene, eye->camera);

    rut_camera_set_near_plane(eye->camera_component,
                              rut_camera_get_near_plane(camera_component));
    rut_camera_set_far_plane(eye->camera_component,
                             rut_camera_get_far_plane(camera_component));

    rut_camera_set_zoom(eye->camera_component,
                        rut_camera_get_zoom(camera_component));

    rig_entity_set_position(eye->camera, rig_entity_get_position(camera));
    rig_entity_set_scale(eye->camera, rig_entity_get_scale(camera));
    //rig_entity_set_rotation(eye->camera, rig_entity_get_rotation(camera));

    eye->head_pose = ovrHmd_GetHmdPosePerEye(view->hmd, eye->type);

    /* TODO: double check that OVR quaternions are defined in exactly
     * the same way... */
    orientation.w = eye->head_pose.Orientation.w;
    orientation.x = eye->head_pose.Orientation.x;
    orientation.y = -eye->head_pose.Orientation.y;
    orientation.z = -eye->head_pose.Orientation.z;

    cg_quaternion_invert(&orientation);

    rig_entity_set_rotation(eye->camera, &orientation);

    /* TODO: apply inter-oculur transform to separate eyes */

    rig_entity_set_camera_view_from_transform(eye->camera);

    rig_paint_ctx->_parent.camera = eye->camera_component;

    cg_framebuffer_clear4f(rut_camera_get_framebuffer(eye->camera_component),
                           CG_BUFFER_BIT_COLOR | CG_BUFFER_BIT_DEPTH |
                           CG_BUFFER_BIT_STENCIL,
                           0, 0, 0, 1);
#if 0
    rut_camera_flush(eye->camera_component);
    cg_primitive_draw(view->debug_triangle,
                      rut_camera_get_framebuffer(eye->camera_component),
                      view->debug_pipeline);
    rut_camera_end_frame(eye->camera_component);
#endif
    rig_renderer_paint_camera(rig_paint_ctx, eye->camera);
}

static void
composite_eye(rig_camera_view_t *view,
              cg_framebuffer_t *fb,
              struct eye *eye)
{
    ovrMatrix4f timewarp_matrices[2];

    ovrHmd_GetEyeTimewarpMatrices(view->hmd, eye->type,
                                  eye->head_pose, timewarp_matrices);

    float identity[] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    cg_pipeline_set_uniform_matrix(eye->distort_pipeline,
                                   eye->eye_rotation_start_loc,
                                   4, /* dimensions */
                                   1, /* count */
                                   true, /* transpose as ovr matrics
                                            are row major */
                                   //(float *)&timewarp_matrices[0]);
                                   identity);
    cg_pipeline_set_uniform_matrix(eye->distort_pipeline,
                                   eye->eye_rotation_end_loc,
                                   4, /* dimensions */
                                   1, /* count */
                                   true, /* transpose as ovr matrics
                                            are row major */
                                   //(float *)&timewarp_matrices[1]);
                                   identity);

    cg_primitive_draw(eye->distortion_prim, fb, eye->distort_pipeline);
}

static void
vr_swap_buffers_hook(cg_framebuffer_t *fb,
                     void *data)
{
    rig_camera_view_t *view = data;

    cg_onscreen_swap_buffers(fb);

    /* FIXME: we should have a more specific way of asserting that we
     * only call ovrHmd_EndFrameTiming() if once we have started
     * a frame... */
    if (!view->ui)
        return;

    /* XXX: check how this interacts with cogl's frame complete
     * notifications; we shouldn't need to now wait for a swap notify
     * from the X server but it wouldn't be surprising if we do in
     * fact end up delayed waiting for the event from X...
     *
     * XXX: check what Begin/EndTiming is assuming about how the
     * relationship between finishing and the vblank period and
     * see if we can improve the timing apis...
     */
    cg_framebuffer_finish(fb);

    ovrHmd_EndFrameTiming(view->hmd);

    /* XXX: ovrHmd_EndFrame() was a useful starting poiny when determining
     * how to use the latency testing apis... */
    ovrHmd_GetMeasuredLatencyTest2(view->hmd);

    rut_shell_queue_redraw(view->engine->shell);
}
#endif /* ENABLE_OCULUS_RIFT */

void
rig_camera_view_paint(rig_camera_view_t *view,
                      rut_object_t *renderer)
{
    cg_framebuffer_t *fb = view->fb;
    rig_entity_t *camera;
    rut_object_t *camera_component;
    rig_paint_context_t rig_paint_ctx;
    rut_paint_context_t *rut_paint_ctx = &rig_paint_ctx._parent;
    int i;

    if (view->ui == NULL)
        return;

    view->width = cg_framebuffer_get_width(fb);
    view->height = cg_framebuffer_get_height(fb);

#ifdef RIG_EDITOR_ENABLED
    if (!view->play_mode) {
        camera = view->view_camera;
        camera_component = view->view_camera_component;
    } else
#endif /* RIG_EDITOR_ENABLED */
    {
        if (view->play_camera == NULL)
            return;

        camera = view->play_camera;
        camera_component = view->play_camera_component;
    }

    rut_paint_ctx->camera = camera_component;

    rig_paint_ctx.engine = view->engine;
    rig_paint_ctx.renderer = renderer;

    rig_paint_ctx.pass = RIG_PASS_COLOR_BLENDED;
    rig_paint_ctx.enable_dof = view->enable_dof;
    rig_paint_ctx.enable_dof = false;

    if (!rig_engine_vr_mode) {

        rut_camera_set_framebuffer(camera_component, fb);
        rut_camera_set_viewport(camera_component,
                                view->fb_x, view->fb_y,
                                view->width, view->height);
        rig_entity_set_camera_view_from_transform(camera);

        cg_framebuffer_clear4f(fb,
                               CG_BUFFER_BIT_COLOR | CG_BUFFER_BIT_DEPTH |
                               CG_BUFFER_BIT_STENCIL,
                               0, 0, 0, 1);

        rig_renderer_paint_camera(&rig_paint_ctx, camera);

    }
#ifdef ENABLE_OCULUS_RIFT
    else {
        ovrFrameTiming frame_timing = ovrHmd_BeginFrameTiming(view->hmd, 0);

        for (i = 0; i < 2; i++) {
            int eye_idx = view->hmd->EyeRenderOrder[i];
            struct eye *eye = &view->eyes[eye_idx];
            paint_eye(view, &rig_paint_ctx, camera, camera_component, eye);
        }

        ovr_WaitTillTime(frame_timing.TimewarpPointSeconds);

        rut_camera_set_framebuffer(view->composite_camera, fb);
        rut_camera_set_viewport(view->composite_camera,
                                view->fb_x, view->fb_y,
                                view->width, view->height);

        rut_paint_ctx->camera = view->composite_camera;
        rut_camera_flush(view->composite_camera);

#if 1
        for (i = 0; i < 2; i++) {
            int eye_idx = view->hmd->EyeRenderOrder[i];
            struct eye *eye = &view->eyes[eye_idx];
            composite_eye(view, fb, eye);
        }
#endif

        rut_camera_end_frame(view->composite_camera);
    }
#endif
}

rut_type_t rig_camera_view_type;

static void
_rig_camera_view_init_type(void)
{

    static rut_graphable_vtable_t graphable_vtable = { NULL, /* child removed */
                                                       NULL, /* child addded */
                                                       NULL /* parent changed */
    };

    rut_type_t *type = &rig_camera_view_type;
#define TYPE rig_camera_view_t

    rut_type_init(type, C_STRINGIFY(TYPE), _rig_camera_view_free);
    rut_type_add_trait(type,
                       RUT_TRAIT_ID_GRAPHABLE,
                       offsetof(TYPE, graphable),
                       &graphable_vtable);
#undef TYPE
}

static void
transform_ray(cg_matrix_t *transform,
              bool inverse_transform,
              float ray_origin[3],
              float ray_direction[3])
{
    cg_matrix_t inverse, normal_matrix, *m;

    m = transform;
    if (inverse_transform) {
        cg_matrix_get_inverse(transform, &inverse);
        m = &inverse;
    }

    cg_matrix_transform_points(m,
                               3, /* num components for input */
                               sizeof(float) * 3, /* input stride */
                               ray_origin,
                               sizeof(float) * 3, /* output stride */
                               ray_origin,
                               1 /* n_points */);

    cg_matrix_get_inverse(m, &normal_matrix);
    cg_matrix_transpose(&normal_matrix);

    rut_util_transform_normal(&normal_matrix,
                              &ray_direction[0],
                              &ray_direction[1],
                              &ray_direction[2]);
}

typedef struct _pick_context_t {
    rig_camera_view_t *view;
    rig_engine_t *engine;
    rut_object_t *view_camera;
    rut_matrix_stack_t *matrix_stack;
    float x;
    float y;
    float *ray_origin;
    float *ray_direction;
    rig_entity_t *selected_entity;
    float selected_distance;
    int selected_index;
} pick_context_t;

static rut_traverse_visit_flags_t
entitygraph_pre_pick_cb(rut_object_t *object, int depth, void *user_data)
{
    pick_context_t *pick_ctx = user_data;

    if (rut_object_is(object, RUT_TRAIT_ID_TRANSFORMABLE)) {
        const cg_matrix_t *matrix = rut_transformable_get_matrix(object);
        rut_matrix_stack_push(pick_ctx->matrix_stack);
        rut_matrix_stack_multiply(pick_ctx->matrix_stack, matrix);
    }

    if (rut_object_get_type(object) == &rig_entity_type) {
        rig_entity_t *entity = object;
        rig_material_t *material;
        rut_component_t *geometry;
        rut_mesh_t *mesh;
        int index;
        float distance;
        bool hit;
        float transformed_ray_origin[3];
        float transformed_ray_direction[3];
        cg_matrix_t transform;
        rut_object_t *input;

        input = rig_entity_get_component(entity, RUT_COMPONENT_TYPE_INPUT);

        if (input) {
            if (rut_object_is(input, RUT_TRAIT_ID_PICKABLE)) {
                rut_matrix_stack_get(pick_ctx->matrix_stack, &transform);

                if (rut_pickable_pick(input,
                                      pick_ctx->view_camera,
                                      &transform,
                                      pick_ctx->x,
                                      pick_ctx->y)) {
                    pick_ctx->selected_entity = entity;
                    return RUT_TRAVERSE_VISIT_BREAK;
                } else
                    return RUT_TRAVERSE_VISIT_CONTINUE;
            } else {
                geometry = rig_entity_get_component(
                    entity, RUT_COMPONENT_TYPE_GEOMETRY);
            }
        } else {
            if (!pick_ctx->view->play_mode) {
                material = rig_entity_get_component(
                    entity, RUT_COMPONENT_TYPE_MATERIAL);
                if (!material || !rig_material_get_visible(material))
                    return RUT_TRAVERSE_VISIT_CONTINUE;
            }

            geometry =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);
        }

        /* Get a model we can pick against */
        if (!(geometry && rut_object_is(geometry, RUT_TRAIT_ID_MESHABLE) &&
              (mesh = rut_meshable_get_mesh(geometry))))
            return RUT_TRAVERSE_VISIT_CONTINUE;

        /* transform the ray into the model space */
        memcpy(transformed_ray_origin, pick_ctx->ray_origin, 3 * sizeof(float));
        memcpy(transformed_ray_direction,
               pick_ctx->ray_direction,
               3 * sizeof(float));

        rut_matrix_stack_get(pick_ctx->matrix_stack, &transform);

        transform_ray(&transform,
                      true, /* inverse of the transform */
                      transformed_ray_origin,
                      transformed_ray_direction);

#if 0
        c_debug ("transformed ray %f,%f,%f %f,%f,%f\n",
                 transformed_ray_origin[0],
                 transformed_ray_origin[1],
                 transformed_ray_origin[2],
                 transformed_ray_direction[0],
                 transformed_ray_direction[1],
                 transformed_ray_direction[2]);
#endif

        /* intersect the transformed ray with the model engine */
        hit = rut_util_intersect_mesh(mesh,
                                      transformed_ray_origin,
                                      transformed_ray_direction,
                                      &index,
                                      &distance);

        if (hit) {
            const cg_matrix_t *view =
                rut_camera_get_view_transform(pick_ctx->view_camera);
            float w = 1;

            /* to compare intersection distances we find the actual point of ray
             * intersection in model coordinates and transform that into eye
             * coordinates */

            transformed_ray_direction[0] *= distance;
            transformed_ray_direction[1] *= distance;
            transformed_ray_direction[2] *= distance;

            transformed_ray_direction[0] += transformed_ray_origin[0];
            transformed_ray_direction[1] += transformed_ray_origin[1];
            transformed_ray_direction[2] += transformed_ray_origin[2];

            cg_matrix_transform_point(&transform,
                                      &transformed_ray_direction[0],
                                      &transformed_ray_direction[1],
                                      &transformed_ray_direction[2],
                                      &w);
            cg_matrix_transform_point(view,
                                      &transformed_ray_direction[0],
                                      &transformed_ray_direction[1],
                                      &transformed_ray_direction[2],
                                      &w);
            distance = transformed_ray_direction[2];

            if (distance > pick_ctx->selected_distance) {
                pick_ctx->selected_entity = entity;
                pick_ctx->selected_distance = distance;
                pick_ctx->selected_index = index;
            }
        }
    }

    return RUT_TRAVERSE_VISIT_CONTINUE;
}

static rut_traverse_visit_flags_t
entitygraph_post_pick_cb(rut_object_t *object, int depth, void *user_data)
{
    if (rut_object_is(object, RUT_TRAIT_ID_TRANSFORMABLE)) {
        pick_context_t *pick_ctx = user_data;
        rut_matrix_stack_pop(pick_ctx->matrix_stack);
    }

    return RUT_TRAVERSE_VISIT_CONTINUE;
}

static rig_entity_t *
pick(rig_camera_view_t *view,
     rut_object_t *view_camera,
     float x,
     float y,
     float ray_origin[3],
     float ray_direction[3])
{
    rig_engine_t *engine = view->engine;
    pick_context_t pick_ctx;

    pick_ctx.view = view;
    pick_ctx.engine = engine;
    pick_ctx.view_camera = view_camera;
    pick_ctx.matrix_stack = view->matrix_stack;
    pick_ctx.x = x;
    pick_ctx.y = y;
    pick_ctx.selected_distance = -FLT_MAX;
    pick_ctx.selected_entity = NULL;
    pick_ctx.ray_origin = ray_origin;
    pick_ctx.ray_direction = ray_direction;

    rut_graphable_traverse(view->ui->scene,
                           RUT_TRAVERSE_DEPTH_FIRST,
                           entitygraph_pre_pick_cb,
                           entitygraph_post_pick_cb,
                           &pick_ctx);

#if 0
    if (pick_ctx.selected_entity)
    {
        c_message("Hit entity, triangle #%d, distance %.2f",
                  pick_ctx.selected_index, pick_ctx.selected_distance);
    }
#endif

    return pick_ctx.selected_entity;
}

#ifdef ENABLE_OCULUS_RIFT
static void
deinit_vr(rig_camera_view_t *view)
{
    rut_object_unref(view->composite_camera);

    if (view->hmd)
        ovrHmd_Destroy(view->hmd);
    ovr_Shutdown();
}

static void
create_eye_distortion_mesh(rig_camera_view_t *view, struct eye *eye)
{
    cg_device_t *dev = view->engine->shell->cg_device;

    ovrDistortionMesh mesh_data;

    ovrHmd_CreateDistortionMesh(view->hmd,
                                eye->type,
                                eye->fov,
                                (ovrDistortionCap_Chromatic |
                                 ovrDistortionCap_TimeWarp),
                                &mesh_data);

    eye->attrib_buf = cg_attribute_buffer_new(dev,
                                              (sizeof(ovrDistortionVertex) *
                                               mesh_data.VertexCount),
                                              mesh_data.pVertexData);

    eye->attribs[0] = cg_attribute_new(eye->attrib_buf,
                                       "cg_position_in",
                                       sizeof(ovrDistortionVertex),
                                       offsetof(ovrDistortionVertex, ScreenPosNDC),
                                       2, /* n components */
                                       CG_ATTRIBUTE_TYPE_FLOAT);
    eye->attribs[1] = cg_attribute_new(eye->attrib_buf,
                                       "warp_factor_in",
                                       sizeof(ovrDistortionVertex),
                                       offsetof(ovrDistortionVertex, TimeWarpFactor),
                                       1, /* n components */
                                       CG_ATTRIBUTE_TYPE_FLOAT);
    eye->attribs[2] = cg_attribute_new(eye->attrib_buf,
                                       "vignette_factor_in",
                                       sizeof(ovrDistortionVertex),
                                       offsetof(ovrDistortionVertex, VignetteFactor),
                                       1, /* n components */
                                       CG_ATTRIBUTE_TYPE_FLOAT);
    eye->attribs[3] = cg_attribute_new(eye->attrib_buf,
                                       "tan_eye_angles_r_in",
                                       sizeof(ovrDistortionVertex),
                                       offsetof(ovrDistortionVertex, TanEyeAnglesR),
                                       2, /* n components */
                                       CG_ATTRIBUTE_TYPE_FLOAT);
    eye->attribs[4] = cg_attribute_new(eye->attrib_buf,
                                       "tan_eye_angles_g_in",
                                       sizeof(ovrDistortionVertex),
                                       offsetof(ovrDistortionVertex, TanEyeAnglesG),
                                       2, /* n components */
                                       CG_ATTRIBUTE_TYPE_FLOAT);
    eye->attribs[5] = cg_attribute_new(eye->attrib_buf,
                                       "tan_eye_angles_b_in",
                                       sizeof(ovrDistortionVertex),
                                       offsetof(ovrDistortionVertex, TanEyeAnglesB),
                                       2, /* n components */
                                       CG_ATTRIBUTE_TYPE_FLOAT);

    eye->index_buf = cg_index_buffer_new(dev, 2 * mesh_data.IndexCount);

    cg_buffer_set_data(eye->index_buf,
                       0, /* offset */
                       mesh_data.pIndexData,
                       2 * mesh_data.IndexCount,
                       NULL); /* exception */

    eye->indices = cg_indices_new_for_buffer(CG_INDICES_TYPE_UNSIGNED_SHORT,
                                             eye->index_buf,
                                             0); /* offset */

    eye->distortion_prim =
        cg_primitive_new_with_attributes(CG_VERTICES_MODE_TRIANGLES,
                                         mesh_data.VertexCount,
                                         eye->attribs,
                                         6); /* n attributes */

    cg_primitive_set_indices(eye->distortion_prim,
                             eye->indices,
                             mesh_data.IndexCount);

    ovrHmd_DestroyDistortionMesh(&mesh_data);
}

static void
init_vr(rig_camera_view_t *view)
{
    struct eye *left_eye = &view->eyes[RIG_EYE_LEFT];
    struct eye *right_eye = &view->eyes[RIG_EYE_RIGHT];
    cg_device_t *dev = view->engine->shell->cg_device;
    cg_vertex_p3c4_t triangle_vertices[] = {
        {  0,    500, -500, 0xff, 0x00, 0x00, 0xff },
        { -500, -500, -500, 0x00, 0xff, 0x00, 0xff },
        {  500, -500, -500, 0x00, 0x00, 0xff, 0xff }
    };
    int i;

    view->engine->frontend->swap_buffers_hook = vr_swap_buffers_hook;
    view->engine->frontend->swap_buffers_hook_data = view;

    ovr_Initialize();

    view->hmd = ovrHmd_Create(0);

    if (!view->hmd) {
        c_warning("Failed to initialize a head mounted display\n"
                  "Creating dummy DK2 device...");

        view->hmd = ovrHmd_CreateDebug(ovrHmd_DK2);
        if (!view->hmd) {
            c_error("Failed to create dummy DK2 device\n");
            goto cleanup;
        }
    }

    c_message("Headset type = %s\n", view->hmd->ProductName);


    ovrHmd_SetEnabledCaps(view->hmd,
                          (ovrHmdCap_DynamicPrediction
                           //| ovrHmdCap_LowPersistence
                           ));

    ovrHmd_ConfigureTracking(view->hmd,
                             ovrTrackingCap_Orientation |
                             ovrTrackingCap_MagYawCorrection |
                             ovrTrackingCap_Position, /* supported */
                             0); /* required */

    view->composite_camera = rig_camera_new(view->engine,
                                            -1, -1, /* viewport w/h */
                                            NULL);  /* fb */
    rut_camera_set_projection_mode(view->composite_camera,
                                   RUT_PROJECTION_NDC);
    rut_camera_set_clear(view->composite_camera, false);

    view->debug_triangle = cg_primitive_new_p3c4(
        dev, CG_VERTICES_MODE_TRIANGLES, 3, triangle_vertices);
    view->debug_pipeline = cg_pipeline_new(dev);
    cg_pipeline_set_blend(view->debug_pipeline, "RGBA = ADD(SRC_COLOR, 0)", NULL);

    memset(view->eyes, 0, sizeof(view->eyes));

    left_eye->type = RIG_EYE_LEFT;
#if 0
    left_eye->x0 = -1;
    left_eye->y0 = 1;
    left_eye->x1 = 0;
    left_eye->y1 = -1;
#endif
    left_eye->viewport[0] = 0;
    left_eye->viewport[1] = 0;
    left_eye->viewport[2] = view->hmd->Resolution.w / 2;
    left_eye->viewport[3] = view->hmd->Resolution.h;

    right_eye->type = RIG_EYE_RIGHT;
#if 0
    right_eye->x0 = 0;
    right_eye->y0 = 1;
    right_eye->x1 = 1;
    right_eye->y1 = -1;
#endif
    right_eye->viewport[0] = (view->hmd->Resolution.w + 1) / 2;
    right_eye->viewport[1] = 0;
    right_eye->viewport[2] = view->hmd->Resolution.w / 2;
    right_eye->viewport[3] = view->hmd->Resolution.h;

    for (i = 0; i < 2; i++) {
        struct eye *eye = &view->eyes[i];
        ovrSizei recommended_size;
        ovrRecti tex_viewport;
        ovrVector2f uv_scale_offset[2];
        cg_snippet_t *snippet;

        eye->fov = view->hmd->DefaultEyeFov[i];

        recommended_size =
            ovrHmd_GetFovTextureSize(view->hmd, eye->type, eye->fov,
                                     1.0 /* pixels per display pixel */);

        eye->tex = cg_texture_2d_new_with_size(dev,
                                               recommended_size.w,
                                               recommended_size.h);
        eye->fb = cg_offscreen_new_with_texture(eye->tex);
        cg_framebuffer_allocate(eye->fb, NULL);

        eye->render_desc = ovrHmd_GetRenderDesc(view->hmd, eye->type, eye->fov);

        tex_viewport.Size = recommended_size;
        tex_viewport.Pos.x = 0;
        tex_viewport.Pos.y = 0;

        /* XXX: The size and viewport this api expects are the size of
         * the eye render target and the viewport used when rendering
         * the eye. I.e. not the size of the final destination
         * framebuffer or viewport used when finally compositing the
         * eyes with mesh distortion. */
        ovrHmd_GetRenderScaleAndOffset(eye->fov, recommended_size, tex_viewport,
                                       uv_scale_offset);

        eye->eye_to_source_uv_scale[0] = uv_scale_offset[0].x;
        eye->eye_to_source_uv_scale[1] = uv_scale_offset[0].y;

        eye->eye_to_source_uv_offset[0] = uv_scale_offset[1].x;
        eye->eye_to_source_uv_offset[1] = uv_scale_offset[1].y;

        eye->camera_component = rig_camera_new(view->engine,
                                               recommended_size.w,
                                               recommended_size.h,
                                               eye->fb);
        rut_camera_set_clear(eye->camera_component, false);
        rut_camera_set_projection_mode(eye->camera_component,
                                       RUT_PROJECTION_ASYMMETRIC_PERSPECTIVE);

#define R_TO_D(X) ((X)*(180.0f / C_PI))
        rut_camera_set_asymmetric_field_of_view(eye->camera_component,
                                                R_TO_D(atanf(eye->fov.LeftTan)),
                                                R_TO_D(atanf(eye->fov.RightTan)),
                                                R_TO_D(atanf(eye->fov.DownTan)),
                                                R_TO_D(atanf(eye->fov.UpTan)));
#undef R_TO_D

        eye->camera = rig_entity_new(view->engine->shell);
        rig_entity_add_component(eye->camera, eye->camera_component);

        eye->distort_pipeline = cg_pipeline_new(dev);
        cg_pipeline_set_layer_texture(eye->distort_pipeline, 0, eye->tex);
        cg_pipeline_set_blend(eye->distort_pipeline, "RGBA = ADD(SRC_COLOR, 0)", NULL);

        snippet = cg_snippet_new(CG_SNIPPET_HOOK_VERTEX,
                                 "uniform vec2 eye_to_source_uv_scale;\n"
                                 "uniform vec2 eye_to_source_uv_offset;\n"
                                 "uniform mat4 eye_rotation_start;\n"
                                 "uniform mat4 eye_rotation_end;\n"
                                 "\n"
                                 "in vec2 tan_eye_angles_r_in;\n"
                                 "in vec2 tan_eye_angles_g_in;\n"
                                 "in vec2 tan_eye_angles_b_in;\n"
                                 "out vec2 tex_coord_r;\n"
                                 "out vec2 tex_coord_g;\n"
                                 "out vec2 tex_coord_b;\n"
                                 "in float warp_factor_in;\n"
                                 "in float vignette_factor_in;\n"
                                 "out float vignette_factor;\n"
                                 "\n"
                                 "vec2 timewarp(vec2 coord, mat4 rot)\n"
                                 "{\n"
                                 //"  vec3 transformed = (rot * vec4(coord.xy, 1.0, 1.0)).xyz;\n"
                                 "  vec3 transformed = vec3(coord.xy, 1.0);\n"
                                 "  vec2 flattened = transformed.xy / transformed.z;\n"
                                 "\n"
                                 "  return eye_to_source_uv_scale * flattened + eye_to_source_uv_offset;\n"
                                 "}\n"
                                 ,
                                 NULL); /* post */
        cg_snippet_set_replace(snippet,
                               "  mat4 lerped_eye_rot = (eye_rotation_start * (1.0 - warp_factor_in)) + \n"
                               "                        (eye_rotation_end * warp_factor_in);\n"
                               "  tex_coord_r = timewarp(tan_eye_angles_r_in, lerped_eye_rot);\n"
                               "  tex_coord_g = timewarp(tan_eye_angles_g_in, lerped_eye_rot);\n"
                               "  tex_coord_b = timewarp(tan_eye_angles_b_in, lerped_eye_rot);\n"
                               //"  tex_coord_r = (cg_position_in.xy * 0.5) + 0.5;\n"
                               //"  tex_coord_g = (cg_position_in.xy * 0.5) + 1.0;\n"
                               //"  tex_coord_b = (cg_position_in.xy * 0.5) + 1.0;\n"
                               //"  tex_coord_g = tan_eye_angles_g_in;\n"
                               //"  tex_coord_b = tan_eye_angles_b_in;\n"
                               "  vignette_factor = vignette_factor_in;\n"
                               "  cg_position_out = vec4(cg_position_in.xy, 0.5, 1.0);\n"
                              );
        cg_pipeline_add_snippet(eye->distort_pipeline, snippet);
        cg_object_unref(snippet);

        snippet = cg_snippet_new(CG_SNIPPET_HOOK_FRAGMENT,
                                 "in vec2 tex_coord_r;\n"
                                 "in vec2 tex_coord_g;\n"
                                 "in vec2 tex_coord_b;\n"
                                 "in float vignette_factor;\n"
                                 ,
                                 NULL); /* post */
        cg_snippet_set_replace(snippet,
                               //"  cg_color_out = vec4(texture(cg_sampler0, tex_coord_r).rgb, 1.0);\n"
                               "  float R = cg_texture_lookup0(cg_sampler0, vec4(tex_coord_r, 0.0, 0.0)).r;\n"
                               "  float G = cg_texture_lookup0(cg_sampler0, vec4(tex_coord_g, 0.0, 0.0)).g;\n"
                               "  float B = cg_texture_lookup0(cg_sampler0, vec4(tex_coord_b, 0.0, 0.0)).b;\n"
                               "  cg_color_out = vignette_factor * vec4(R, G, B, 1.0);\n"
                               //"  cg_color_out = vec4(R, G, B, 1.0);\n"
                               //"  cg_color_out = vec4(1.0, 0.0, 0.0, 1.0);\n"
                               );
        cg_pipeline_add_snippet(eye->distort_pipeline, snippet);
        cg_object_unref(snippet);

        eye->eye_to_source_uv_scale_loc =
            cg_pipeline_get_uniform_location(eye->distort_pipeline, "eye_to_source_uv_scale");
        eye->eye_to_source_uv_offset_loc =
            cg_pipeline_get_uniform_location(eye->distort_pipeline, "eye_to_source_uv_offset");
        eye->eye_rotation_start_loc =
            cg_pipeline_get_uniform_location(eye->distort_pipeline, "eye_rotation_start");
        eye->eye_rotation_end_loc =
            cg_pipeline_get_uniform_location(eye->distort_pipeline, "eye_rotation_end");

        cg_pipeline_set_uniform_float(eye->distort_pipeline,
                                      eye->eye_to_source_uv_scale_loc,
                                      2, /* n components */
                                      1, /* count */
                                      eye->eye_to_source_uv_scale);

        cg_pipeline_set_uniform_float(eye->distort_pipeline,
                                      eye->eye_to_source_uv_offset_loc,
                                      2, /* n components */
                                      1, /* count */
                                      eye->eye_to_source_uv_offset);

        create_eye_distortion_mesh(view, eye);
    }

    return;

cleanup:
    if (rig_engine_vr_mode)
        deinit_vr(view);
}
#endif /* ENABLE_OCULUS_RIFT */

rig_camera_view_t *
rig_camera_view_new(rig_frontend_t *frontend)
{
    rig_camera_view_t *view = rut_object_alloc0(
        rig_camera_view_t, &rig_camera_view_type, _rig_camera_view_init_type);

    view->frontend = frontend;
    view->engine = frontend->engine;
    view->shell = view->engine->shell;

    rut_graphable_init(view);

    //rut_graphable_add_child(view, view->input_region);

#ifdef ENABLE_OCULUS_RIFT
    if (rig_engine_vr_mode)
        init_vr(view);
#endif

    view->matrix_stack = rut_matrix_stack_new(view->shell);

    view->view_camera = rig_entity_new(view->shell);
    rig_entity_set_label(view->view_camera, "rig:camera");

    view->view_camera_component = rig_camera_new(view->engine,
                                                 -1, /* ortho/vp width */
                                                 -1, /* ortho/vp height */
                                                 NULL);
    rut_camera_set_clear(view->view_camera_component, false);
    rig_entity_add_component(view->view_camera, view->view_camera_component);

    return view;
}

void
rig_camera_view_set_framebuffer(rig_camera_view_t *view,
                                cg_framebuffer_t *fb)
{
    if (view->fb == fb)
        return;

    if (view->fb) {
        cg_object_unref(view->fb);
        view->fb = NULL;
    }

    if (fb)
        view->fb = cg_object_ref(fb);
}

void
set_play_camera(rig_camera_view_t *view, rig_entity_t *play_camera)
{
    if (view->play_camera == play_camera)
        return;

    if (view->play_camera) {
        rut_object_unref(view->play_camera);
        view->play_camera = NULL;
        rut_object_unref(view->play_camera_component);
        view->play_camera_component = NULL;
    }

    if (play_camera) {
        view->play_camera = rut_object_ref(play_camera);
        view->play_camera_component =
            rig_entity_get_component(play_camera, RUT_COMPONENT_TYPE_CAMERA);
        rut_object_ref(view->play_camera_component);
    }
}

void
rig_camera_view_set_ui(rig_camera_view_t *view, rig_ui_t *ui)
{
    if (view->ui == ui)
        return;

    if (view->ui) {
        set_play_camera(view, NULL);
        rut_graphable_remove_child(view->view_camera);
        rut_shell_remove_input_camera(
            view->shell, view->view_camera_component, view->ui->scene);
    }

    /* XXX: to avoid having a circular reference we don't take a
     * reference on the ui... */
    view->ui = ui;

    if (ui) {
        rut_shell_add_input_camera(
            view->shell, view->view_camera_component, ui->scene);
        set_play_camera(view, ui->play_camera);
        rut_graphable_add_child(ui->scene, view->view_camera);
        init_camera_from_camera(view->view_camera, view->play_camera);

        view->origin[0] = 0;//engine->device_width / 2;
        view->origin[1] = 0;//engine->device_height / 2;
        view->origin[2] = 0;
    }

    rut_shell_queue_redraw(view->shell);
}
