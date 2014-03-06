/*
 * Rut
 *
 * Rig Utilities
 *
 * Copyright (C) 2013  Intel Corporation.
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

#include <glib.h>

#include "rut-interfaces.h"
#include "rut-fixed.h"

struct _RutFixed
{
  RutObjectBase _base;

  RutContext *context;

  RutList preferred_size_cb_list;

  float width;
  float height;

  RutGraphableProps graphable;
};

static void
_rut_fixed_free (void *object)
{
  RutFixed *fixed = object;

  rut_closure_list_disconnect_all (&fixed->preferred_size_cb_list);

  rut_graphable_destroy (fixed);

  rut_object_free (RutFixed, object);
}

static void
rut_fixed_get_preferred_width (void *sizable,
                               float for_height,
                               float *min_width_p,
                               float *natural_width_p)
{
  RutFixed *fixed = sizable;

  if (min_width_p)
    *min_width_p = fixed->width;
  if (natural_width_p)
    *natural_width_p = fixed->width;
}

static void
rut_fixed_get_preferred_height (void *sizable,
                                float for_width,
                                float *min_height_p,
                                float *natural_height_p)
{
  RutFixed *fixed = sizable;

  if (min_height_p)
    *min_height_p = fixed->height;
  if (natural_height_p)
    *natural_height_p = fixed->height;
}

RutType rut_fixed_type;

static void
_rut_fixed_init_type (void)
{

  static RutGraphableVTable graphable_vtable = {
      NULL, /* child remove */
      NULL, /* child add */
      NULL /* parent changed */
  };

  static RutSizableVTable sizable_vtable = {
      rut_fixed_set_size,
      rut_fixed_get_size,
      rut_fixed_get_preferred_width,
      rut_fixed_get_preferred_height,
      NULL /* add_preferred_size_callback */
  };

  RutType *type = &rut_fixed_type;
#define TYPE RutFixed

  rut_type_init (type, G_STRINGIFY (TYPE), _rut_fixed_free);
  rut_type_add_trait (type,
                      RUT_TRAIT_ID_GRAPHABLE,
                      offsetof (TYPE, graphable),
                      &graphable_vtable);
  rut_type_add_trait (type,
                      RUT_TRAIT_ID_SIZABLE,
                      0, /* no implied properties */
                      &sizable_vtable);

#undef TYPE
}

RutFixed *
rut_fixed_new (RutContext *ctx,
               float width,
               float height)
{
  RutFixed *fixed =
    rut_object_alloc0 (RutFixed, &rut_fixed_type, _rut_fixed_init_type);



  fixed->context = ctx;

  rut_list_init (&fixed->preferred_size_cb_list);

  rut_graphable_init (fixed);

  fixed->width = width;
  fixed->height = height;

  return fixed;
}

void
rut_fixed_set_width (RutFixed *fixed, float width)
{
  rut_fixed_set_size (fixed, width, fixed->height);
}

void
rut_fixed_set_height (RutFixed *fixed, float height)
{
  rut_fixed_set_size (fixed, fixed->width, height);
}

void
rut_fixed_set_size (RutObject *self,
                    float width,
                    float height)
{
  RutFixed *fixed = self;

  if (fixed->width == width && fixed->height == height)
    return;

  fixed->width = width;
  fixed->height = height;

  rut_closure_list_invoke (&fixed->preferred_size_cb_list,
                           RutSizablePreferredSizeCallback,
                           fixed);
}

void
rut_fixed_get_size (RutObject *self,
                    float *width,
                    float *height)
{
  RutFixed *fixed = self;
  *width = fixed->width;
  *height = fixed->height;
}

void
rut_fixed_add_child (RutFixed *fixed, RutObject *child)
{
  g_return_if_fail (rut_object_get_type (fixed) == &rut_fixed_type);
  rut_graphable_add_child (fixed, child);
}

void
rut_fixed_remove_child (RutFixed *fixed, RutObject *child)
{
  g_return_if_fail (rut_object_get_type (fixed) == &rut_fixed_type);
  rut_graphable_remove_child (child);
}
