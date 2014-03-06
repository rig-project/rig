/*
 * Rut
 *
 * Copyright (C) 2012  Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "rut-camera.h"

RutContext *
rut_camera_get_context (RutObject *object)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  return vtable->get_context (object);
}

void
rut_camera_set_background_color4f (RutObject *object,
                                   float red,
                                   float green,
                                   float blue,
                                   float alpha)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_background_color4f (object, red, green, blue, alpha);
}

void
rut_camera_set_background_color (RutObject *object,
                                 const CoglColor *color)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_background_color (object, color);
}

const CoglColor *
rut_camera_get_background_color (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return &camera->bg_color;
}

void
rut_camera_set_clear (RutObject *object,
                      bool clear)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_clear (object, clear);
}

CoglFramebuffer *
rut_camera_get_framebuffer (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return camera->fb;
}

void
rut_camera_set_framebuffer (RutObject *object,
                            CoglFramebuffer *framebuffer)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_framebuffer (object, framebuffer);
}

void
rut_camera_set_viewport (RutObject *object,
                         float x,
                         float y,
                         float width,
                         float height)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_viewport (object, x, y, width, height);
}

void
rut_camera_set_viewport_x (RutObject *object,
                           float x)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_viewport_x (object, x);
}

void
rut_camera_set_viewport_y (RutObject *object,
                           float y)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_viewport_y (object, y);
}

void
rut_camera_set_viewport_width (RutObject *object,
                               float width)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_viewport_width (object, width);
}

void
rut_camera_set_viewport_height (RutObject *object,
                                float height)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_viewport_height (object, height);
}

const float *
rut_camera_get_viewport (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return camera->viewport;
}

const CoglMatrix *
rut_camera_get_projection (RutObject *object)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  return vtable->get_projection (object);
}

void
rut_camera_set_near_plane (RutObject *object,
                           float near)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_near_plane (object, near);
}

float
rut_camera_get_near_plane (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return camera->near;
}

void
rut_camera_set_far_plane (RutObject *object,
                          float far)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_far_plane (object, far);
}

float
rut_camera_get_far_plane (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return camera->far;
}

RutProjection
rut_camera_get_projection_mode (RutObject *object)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  return vtable->get_projection_mode (object);
}

void
rut_camera_set_projection_mode (RutObject *object,
                                RutProjection projection)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_projection_mode (object, projection);
}

void
rut_camera_set_field_of_view (RutObject *object,
                              float fov)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_field_of_view (object, fov);
}

float
rut_camera_get_field_of_view (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return camera->fov;
}

void
rut_camera_set_orthographic_coordinates (RutObject *object,
                                         float x1,
                                         float y1,
                                         float x2,
                                         float y2)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_orthographic_coordinates (object, x1, y1, x2, y2);
}

void
rut_camera_get_orthographic_coordinates (RutObject *object,
                                         float *x1,
                                         float *y1,
                                         float *x2,
                                         float *y2)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  *x1 = camera->x1;
  *y1 = camera->y1;
  *x2 = camera->x2;
  *y2 = camera->y2;
}

const CoglMatrix *
rut_camera_get_inverse_projection (RutObject *object)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  return vtable->get_inverse_projection (object);
}

void
rut_camera_set_view_transform (RutObject *object,
                               const CoglMatrix *view)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_view_transform (object, view);
}

const CoglMatrix *
rut_camera_get_view_transform (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return &camera->view;
}

const CoglMatrix *
rut_camera_get_inverse_view_transform (RutObject *object)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  return vtable->get_inverse_view_transform (object);
}

const CoglMatrix *
rut_camera_get_input_transform (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return &camera->input_transform;
}

void
rut_camera_set_input_transform (RutObject *object,
                                const CoglMatrix *input_transform)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_input_transform (object, input_transform);
}

void
rut_camera_flush (RutObject *object)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->flush (object);
}

void
rut_camera_suspend (RutObject *object)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->suspend (object);
}

void
rut_camera_resume (RutObject *object)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->resume (object);
}

void
rut_camera_end_frame (RutObject *object)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->end_frame (object);
}

void
rut_camera_add_input_region (RutObject *object,
                             RutInputRegion *region)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->add_input_region (object, region);
}

void
rut_camera_remove_input_region (RutObject *object,
                                RutInputRegion *region)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->remove_input_region (object, region);
}

GList *
rut_camera_get_input_regions (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);
  return camera->input_regions;
}

bool
rut_camera_transform_window_coordinate (RutObject *object,
                                        float *x,
                                        float *y)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  return vtable->transform_window_coordinate (object, x, y);
}

void
rut_camera_unproject_coord (RutObject *object,
                            const CoglMatrix *modelview,
                            const CoglMatrix *inverse_modelview,
                            float object_coord_z,
                            float *x,
                            float *y)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->unproject_coord (object,
                           modelview,
                           inverse_modelview,
                           object_coord_z,
                           x, y);
}

CoglPrimitive *
rut_camera_create_frustum_primitive (RutObject *object)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  return vtable->create_frustum_primitive (object);
}

void
rut_camera_set_focal_distance (RutObject *object,
                               float focal_distance)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_focal_distance (object, focal_distance);
}

float
rut_camera_get_focal_distance (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return camera->focal_distance;
}

void
rut_camera_set_depth_of_field (RutObject *object,
                               float depth_of_field)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_depth_of_field (object, depth_of_field);
}

float
rut_camera_get_depth_of_field (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return camera->depth_of_field;
}

void
rut_camera_set_zoom (RutObject *object,
                     float zoom)
{
  RutCameraVTable *vtable =
    rut_object_get_vtable (object, RUT_TRAIT_ID_CAMERA);

  vtable->set_zoom (object, zoom);
}

float
rut_camera_get_zoom (RutObject *object)
{
  RutCameraProps *camera =
    rut_object_get_properties (object, RUT_TRAIT_ID_CAMERA);

  return camera->zoom;
}

