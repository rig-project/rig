/*
 * Rut
 *
 * Copyright (C) 2012 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include <cogl/cogl.h>
#include <string.h>
#include <math.h>

#include "rut.h"
#include "rut-inspector.h"
#include "rut-prop-inspector.h"
#include "rut-vec3-slider.h"
#include "rut-number-slider.h"
#include "rut-drop-down.h"

#define RUT_INSPECTOR_EDGE_GAP 5
#define RUT_INSPECTOR_PROPERTY_GAP 5

#define RUT_INSPECTOR_N_COLUMNS 1

typedef struct
{
  RutObject *control;
  RutTransform *transform;
  RutDragBin *drag_bin;
  RutProperty *source_prop;
  RutProperty *target_prop;

  /* A pointer is stored back to the inspector so that we can use a
   * pointer to this data directly as the callback data for the
   * property binding */
  RutInspector *inspector;
} RutInspectorPropertyData;

struct _RutInspector
{
  RutObjectProps _parent;

  RutContext *context;
  GList *objects;

  RutPaintableProps paintable;
  RutGraphableProps graphable;

  int n_props;
  int n_rows;
  RutInspectorPropertyData *prop_data;

  int width;
  int height;

  RutInspectorCallback property_changed_cb;
  RutInspectorControlledCallback controlled_changed_cb;
  void *user_data;

  int ref_count;
};

RutType rut_inspector_type;

static void
_rut_inspector_free (void *object)
{
  RutInspector *inspector = object;
  RutObject *reference_object = inspector->objects->data;

  rut_refable_unref (inspector->context);

  if (rut_object_is (reference_object, RUT_INTERFACE_ID_REF_COUNTABLE))
    g_list_foreach (inspector->objects, (GFunc)rut_refable_unref, NULL);
  g_list_free (inspector->objects);
  inspector->objects = NULL;

  g_free (inspector->prop_data);

  rut_graphable_destroy (inspector);

  g_slice_free (RutInspector, inspector);
}

RutRefableVTable _rut_inspector_refable_vtable = {
  rut_refable_simple_ref,
  rut_refable_simple_unref,
  _rut_inspector_free
};

static RutGraphableVTable _rut_inspector_graphable_vtable = {
  NULL, /* child removed */
  NULL, /* child addded */
  NULL /* parent changed */
};

static void
_rut_inspector_paint (RutObject *object,
                      RutPaintContext *paint_ctx)
{
  /* NOP */
}

RutPaintableVTable _rut_inspector_paintable_vtable = {
  _rut_inspector_paint
};

static void
rut_inspector_set_size (void *object,
                        float width_float,
                        float height_float)
{
  int width = width_float;
  int height = height_float;
  RutInspector *inspector = RUT_INSPECTOR (object);
  float total_width = width - RUT_INSPECTOR_EDGE_GAP * 2;
  float slider_width =
    (total_width - ((RUT_INSPECTOR_N_COLUMNS - 1.0f) *
                    RUT_INSPECTOR_PROPERTY_GAP)) /
    RUT_INSPECTOR_N_COLUMNS;
  float pixel_slider_width = floorf (slider_width);
  float y_pos = RUT_INSPECTOR_EDGE_GAP;
  float row_height = 0.0f;
  int i;

  inspector->width = width;
  inspector->height = height;

  for (i = 0; i < inspector->n_props; i++)
    {
      RutInspectorPropertyData *prop_data = inspector->prop_data + i;
      float preferred_height;

      rut_transform_init_identity (prop_data->transform);
      rut_transform_translate (prop_data->transform,
                               RUT_INSPECTOR_EDGE_GAP +
                               nearbyintf ((slider_width +
                                            RUT_INSPECTOR_PROPERTY_GAP) *
                                           (i % RUT_INSPECTOR_N_COLUMNS)),
                               y_pos,
                               0.0f);

      rut_sizable_get_preferred_height (prop_data->drag_bin,
                                        pixel_slider_width,
                                        NULL,
                                        &preferred_height);

      rut_sizable_set_size (prop_data->drag_bin,
                            pixel_slider_width,
                            preferred_height);

      if (preferred_height > row_height)
        row_height = preferred_height;

      if ((i + 1) % RUT_INSPECTOR_N_COLUMNS == 0)
        {
          y_pos += row_height + RUT_INSPECTOR_PROPERTY_GAP;
          row_height = 0.0f;
        }
    }
}

static void
rut_inspector_get_preferred_width (void *object,
                                   float for_height,
                                   float *min_width_p,
                                   float *natural_width_p)
{
  RutInspector *inspector = RUT_INSPECTOR (object);
  float max_natural_width = 0.0f;
  float max_min_width = 0.0f;
  int i;

  /* Convert the for_height into a height for each row */
  if (for_height >= 0)
    for_height = ((for_height -
                   RUT_INSPECTOR_EDGE_GAP * 2 -
                   (inspector->n_rows - 1) *
                   RUT_INSPECTOR_PROPERTY_GAP) /
                  inspector->n_rows);

  for (i = 0; i < inspector->n_props; i++)
    {
      RutInspectorPropertyData *prop_data = inspector->prop_data + i;
      float min_width;
      float natural_width;

      rut_sizable_get_preferred_width (prop_data->control,
                                       for_height,
                                       &min_width,
                                       &natural_width);

      if (min_width > max_min_width)
        max_min_width = min_width;
      if (natural_width > max_natural_width)
        max_natural_width = natural_width;
    }

  if (min_width_p)
    *min_width_p = (max_min_width * RUT_INSPECTOR_N_COLUMNS +
                    (RUT_INSPECTOR_N_COLUMNS - 1) *
                    RUT_INSPECTOR_PROPERTY_GAP +
                    RUT_INSPECTOR_EDGE_GAP * 2);
  if (natural_width_p)
    *natural_width_p = (max_natural_width * RUT_INSPECTOR_N_COLUMNS +
                        (RUT_INSPECTOR_N_COLUMNS - 1) *
                        RUT_INSPECTOR_PROPERTY_GAP +
                        RUT_INSPECTOR_EDGE_GAP * 2);
}

static void
rut_inspector_get_preferred_height (void *object,
                                    float for_width,
                                    float *min_height_p,
                                    float *natural_height_p)
{
  RutInspector *inspector = RUT_INSPECTOR (object);
  float total_height = 0.0f;
  float row_height = 0.0f;
  int i;

  /* Convert the for_width to the width that each slider will actually
   * get */
  if (for_width >= 0.0f)
    {
      float total_width = for_width - RUT_INSPECTOR_EDGE_GAP * 2;
      float slider_width =
        (total_width - ((RUT_INSPECTOR_N_COLUMNS - 1.0f) *
                        RUT_INSPECTOR_PROPERTY_GAP)) /
        RUT_INSPECTOR_N_COLUMNS;

      for_width = floorf (slider_width);
    }

  for (i = 0; i < inspector->n_props; i++)
    {
      RutInspectorPropertyData *prop_data = inspector->prop_data + i;
      float natural_height;

      rut_sizable_get_preferred_height (prop_data->control,
                                        for_width,
                                        NULL, /* min_height */
                                        &natural_height);

      if (natural_height > row_height)
        row_height = natural_height;

      if ((i + 1) % RUT_INSPECTOR_N_COLUMNS == 0 ||
          i == inspector->n_props - 1)
        {
          total_height += row_height;
          row_height = 0.0f;
        }
    }

  total_height += (RUT_INSPECTOR_EDGE_GAP * 2 +
                   RUT_INSPECTOR_PROPERTY_GAP *
                   (inspector->n_rows - 1));

  if (min_height_p)
    *min_height_p = total_height;
  if (natural_height_p)
    *natural_height_p = total_height;
}

static void
rut_inspector_get_size (void *object,
                        float *width,
                        float *height)
{
  RutInspector *inspector = RUT_INSPECTOR (object);

  *width = inspector->width;
  *height = inspector->height;
}

static RutSizableVTable _rut_inspector_sizable_vtable = {
  rut_inspector_set_size,
  rut_inspector_get_size,
  rut_inspector_get_preferred_width,
  rut_inspector_get_preferred_height,
  NULL /* add_preferred_size_callback */
};

static void
_rut_inspector_init_type (void)
{
  rut_type_init (&rut_inspector_type, "RigInspector");
  rut_type_add_interface (&rut_inspector_type,
                          RUT_INTERFACE_ID_REF_COUNTABLE,
                          offsetof (RutInspector, ref_count),
                          &_rut_inspector_refable_vtable);
  rut_type_add_interface (&rut_inspector_type,
                          RUT_INTERFACE_ID_PAINTABLE,
                          offsetof (RutInspector, paintable),
                          &_rut_inspector_paintable_vtable);
  rut_type_add_interface (&rut_inspector_type,
                          RUT_INTERFACE_ID_GRAPHABLE,
                          offsetof (RutInspector, graphable),
                          &_rut_inspector_graphable_vtable);
  rut_type_add_interface (&rut_inspector_type,
                          RUT_INTERFACE_ID_SIZABLE,
                          0, /* no implied properties */
                          &_rut_inspector_sizable_vtable);
}

static void
property_changed_cb (RutProperty *primary_target_prop,
                     RutProperty *source_prop,
                     void *user_data)
{
  RutInspectorPropertyData *prop_data = user_data;
  RutInspector *inspector = prop_data->inspector;
  GList *l;

  g_return_if_fail (primary_target_prop == prop_data->target_prop);

  /* Forward the property change to the corresponding property
   * of all objects being inspected... */
  for (l = inspector->objects; l; l = l->next)
    {
      RutProperty *target_prop =
        rut_introspectable_lookup_property (l->data,
                                            primary_target_prop->spec->name);

      inspector->property_changed_cb (target_prop, /* target */
                                      source_prop,
                                      inspector->user_data);
    }
}

static void
controlled_changed_cb (RutProperty *primary_property,
                       CoglBool value,
                       void *user_data)
{
  RutInspectorPropertyData *prop_data = user_data;
  RutInspector *inspector = prop_data->inspector;
  GList *l;

  g_return_if_fail (primary_property == prop_data->target_prop);

  /* Forward the controlled state change to the corresponding property
   * of all objects being inspected... */
  for (l = inspector->objects; l; l = l->next)
    {
      RutProperty *property =
        rut_introspectable_lookup_property (l->data,
                                            primary_property->spec->name);

      inspector->controlled_changed_cb (property,
                                        value,
                                        inspector->user_data);
    }
}

static void
get_all_properties_cb (RutProperty *prop,
                       void *user_data)
{
  GArray *array = user_data;
  RutInspectorPropertyData *prop_data;

  g_array_set_size (array, array->len + 1);
  prop_data = &g_array_index (array,
                              RutInspectorPropertyData,
                              array->len - 1);
  prop_data->target_prop = prop;
}

static void
create_property_controls (RutInspector *inspector)
{
  RutObject *reference_object = inspector->objects->data;
  GArray *props;
  int i;

  props = g_array_new (FALSE, /* not zero terminated */
                       FALSE, /* don't clear */
                       sizeof (RutInspectorPropertyData));

  if (rut_object_is (reference_object, RUT_INTERFACE_ID_INTROSPECTABLE))
    rut_introspectable_foreach_property (reference_object,
                                         get_all_properties_cb,
                                         props);

  inspector->n_props = props->len;
  inspector->n_rows = ((inspector->n_props + RUT_INSPECTOR_N_COLUMNS - 1) /
                       RUT_INSPECTOR_N_COLUMNS);
  inspector->prop_data = ((RutInspectorPropertyData *)
                          g_array_free (props, FALSE));

  for (i = 0; i < inspector->n_props; i++)
    {
      RutInspectorPropertyData *prop_data = inspector->prop_data + i;
      RutObject *control;

      prop_data->inspector = inspector;

      prop_data->transform = rut_transform_new (inspector->context);
      rut_graphable_add_child (inspector, prop_data->transform);

      prop_data->drag_bin = rut_drag_bin_new (inspector->context);
      rut_drag_bin_set_payload (prop_data->drag_bin, inspector);
      rut_graphable_add_child (prop_data->transform, prop_data->drag_bin);
      rut_refable_unref (prop_data->drag_bin);

      control = rut_prop_inspector_new (inspector->context,
                                        prop_data->target_prop,
                                        property_changed_cb,
                                        controlled_changed_cb,
                                        prop_data);

      rut_drag_bin_set_child (prop_data->drag_bin, control);
      rut_refable_unref (control);

      prop_data->control = control;
    }
}

RutInspector *
rut_inspector_new (RutContext *context,
                   GList *objects,
                   RutInspectorCallback user_property_changed_cb,
                   RutInspectorControlledCallback user_controlled_changed_cb,
                   void *user_data)
{
  RutObject *reference_object = objects->data;
  RutInspector *inspector = g_slice_new0 (RutInspector);
  static CoglBool initialized = FALSE;

  if (initialized == FALSE)
    {
      _rut_init ();
      _rut_inspector_init_type ();

      initialized = TRUE;
    }

  inspector->ref_count = 1;
  inspector->context = rut_refable_ref (context);
  inspector->objects = g_list_copy (objects);

  if (rut_object_is (reference_object, RUT_INTERFACE_ID_REF_COUNTABLE))
    g_list_foreach (objects, (GFunc)rut_refable_ref, NULL);

  inspector->property_changed_cb = user_property_changed_cb;
  inspector->controlled_changed_cb = user_controlled_changed_cb;
  inspector->user_data = user_data;

  rut_object_init (&inspector->_parent, &rut_inspector_type);

  rut_paintable_init (inspector);
  rut_graphable_init (inspector);

  create_property_controls (inspector);

  rut_inspector_set_size (inspector, 10, 10);

  return inspector;
}

void
rut_inspector_reload_property (RutInspector *inspector,
                               RutProperty *property)
{
  int i;

  for (i = 0; i < inspector->n_props; i++)
    {
      RutInspectorPropertyData *prop_data = inspector->prop_data + i;

      if (prop_data->target_prop == property)
        rut_prop_inspector_reload_property (prop_data->control);
    }
}

void
rut_inspector_set_property_controlled (RutInspector *inspector,
                                       RutProperty *property,
                                       CoglBool controlled)
{
  int i;

  for (i = 0; i < inspector->n_props; i++)
    {
      RutInspectorPropertyData *prop_data = inspector->prop_data + i;

      if (prop_data->target_prop == property)
        rut_prop_inspector_set_controlled (prop_data->control, controlled);
    }
}
