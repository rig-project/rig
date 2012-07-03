/*
 * Rig
 *
 * A tiny toolkit
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

#include <glib.h>

#include <cogl/cogl.h>
#include <cogl-pango/cogl-pango.h>

/* FIXME: we should have a config.h where things like USE_SDL would
 * be defined instead of defining that in rig.h */
#include "rig.h"

#include "rig-bitmask.h"
#include "rig-global.h"
#include "rig-context.h"
#include "rig-camera-private.h"
#include "rig-transform-private.h"
#include "rig-text.h"
#include "rig-timeline.h"
#include "rig-text-buffer.h"
#include "rig-entity.h"
#include "rig-components.h"

/*
 * Overall issues to keep in mind for a useful and efficient UI scenegraph:
 * (in no particular order)
 *
 * How does it handle batching geometry?
 * - How does it handle tiny primitives that can't be efficiently handled using
 *   the GPU.
 * - How does it reorder primitives to avoid state changes?
 * - How does it avoid redundant overdraw?
 * How does it handle culling?
 * How does it track damage regions?
 * How does it handle incremental screen updates?
 * How does it handle anti-aliasing?
 * How does it handle filter effects; blur, desaturate etc?
 * How does it integrate video efficiently?
 * How does it handle animations?
 * How does it ensure the GL driver can't block the application?
 * How does it ensure the application can't block animations?
 * How flexible is the rendering model?
 *  - Is cloning nodes a core part of the scene graph design?
 *  - Is the graph acyclic, or does it allow recursion?
 *
 * Note: Rig doesn't actually tackle any of these particularly well currently
 * and may never since it's currently just wanted as a minimal toolkit in Cogl
 * for debugging purposes and examples. Never the less they are things to keep
 * in mind when shaping the code on the off-chance that something interesting
 * comes out of it.
 *
 * One quite nice thing about this code is the simple approach to interface
 * oriented programming:
 *
 * - Interfaces are a vtable struct typedef of function pointers that must be
 *   implemented and a struct typedef of per-instance properties that must be
 *   available. (Both are optional)
 *
 * - Types are variables that have a bitmask of supported interfaces and an
 *   array indexable up to the highest offset bit in the bitmask. Each entry
 *   contains a pointer to an interface vtable and byte-offset that can be used
 *   to access interface properties associated with an instance.
 *
 * - The base object just contains a single "type" pointer (which could
 *   potentially be changed dynamically at runtime to add/remove interfaces).
 *
 * - Checking if an object implements an interface as well as calling through
 *   the interface vtable or accessing interface properties can be done in O(1)
 *   time.
 *
 * Interfaces defined currently for Rig are:
 * "RefCountable"
 *  - implies an int ref_count property and ref, unref, free methods
 * "Graphable"
 *  - implies parent and children properties but no methods
 * "PaintBatchable"
 *  - no properties implied but adds set_insert_point and update methods.
 *
 * The Rig rendering model was designed so objects retain drawing primitives
 * and drawing state instead of using immediate mode drawing.
 *
 * The objects in the scenegraph have a very tight integration with the linear
 * "display list" structure used to actually paint. A display list is just
 * a linked list of rendering commands including transformation and primitive
 * drawing commands. Each object that wants to render is expected to maintain
 * a linked list of drawing commands.
 *
 * Objects in the scenegraph wanting to render implement the "PaintBatchable"
 * interface which has two methods "set_insert_point" and "update_batch".  The
 * set_insert_point method gives the object a display list link node which
 * tells it where it can insert its own linked list of commands. The
 * "update_batch" method (not used currently) will be used if an object queues
 * an update and it allows the object to change the commands it has linked into
 * the display list.
 *
 * This design will mean that individual objects may be update completely in
 * isolation without any graph traversal.
 *
 * The main disadvantage is that with no indirection at all then it would be
 * difficult to add a thread boundary for rendering without copying the display
 * list. Later instead of literally manipulating a GList though we could use
 * a rig_display_list api which would allow us to internally queue list
 * manipulations instead of allowing direct access.
 *
 * Transforms around children in the scenegraph will have corresponding "push"
 * "pop" commands in the display list and the "pop" commands will contain a
 * back link to the "push". This means that when dealing with the display list
 * data structure we can walk backwards from any primitive to recover all the
 * transformations apply to the primitive jumping over redundant commands.
 *
 * Another problem with this design compared to having a simple
 * imperative paint method like clutter is that it may be more awkward
 * to support nodes belonging to multiple camera graphs which would
 * each need separate display lists. The interface would need some
 * further work to allow nodes to be associated with multiple cameras.
 *
 * Something else to consider is the very tight coupling between nodes
 * in the graph and the code that paints what they represent.
 * Something I'm keen on experimenting with is having a globally aware
 * scene compositor that owns the whole screen, but can derive the
 * structure of a UI from a scene graph, and input regions may be
 * associated with the graph too.
 *
 */

/* Requirements for batching:
 * - Want to be able to insert commands around children. E.g. for a transform
 *   we want to insert a "push" command, then a transform, then a child's
 *   commands and then a "pop" command. Also we want a parent to be able to
 *   draw things before and after drawing children.
 * - Want to be able to "re-batch" individual objects in isolation so the
 *   cost of batching a new frame scales according to how many changes
 *   there are not by the total scene complexity.
 * - Want to be able to hide an object by removing from a display list
 *   ("un-batching")
 * - We need to think about how transform information is exposed so that it's
 *   possible to efficiently determine the transformation of any batched
 *   primitive. In Clutter we have an "apply_transform" method because
 *   transforms are dealt with in the paint method which is a black box but
 *   the apply_transform approach isn't very extensible (e.g. it can't handle
 *   projection matrix changes.)
 *
 * Can we use a clutter-style imperative paint method?
 *  - The advantage is that it's a natural way for an implementation to
 *    pass control to children and directly pass the display list insert
 *    point too:
 *
 *    void
 *    rig_batchable_insert_batch (RigObject *object,
 *                                RigBatchContext *paint_ctx,
 *                                GList *insert_point);
 *
 *    Having a wrapper function as above like clutter also allows us to
 *    play tricks and not necessarily *actually* paint the child; we
 *    might just change the child's insert point if we know the child
 *    itself hasn't change.
 *
 * Why do we have the apply transform in clutter?
 * - To determine the matrix used for input transformation because the transforms
 *   are dealt with as part of the imperative paint functions which are a black
 *   box.
 * Do we need that here?
 * - If the transforms were handled as nodes in the scenegraph then no we can
 *   just walk up the ancestors of the scenegraph.
 *   - The disadvantage is that some of these transforms are essentially just
 *     implementation details for a particular drawable and there is a question
 *     of who owns the scenegraph so it might not make sense to let objects
 *     expose private transforms in the scenegraph.
 *   - recovering the transforms from the display list would be possible but
 *   that could be quite inefficient if we don't find a way to avoid walking
 *   over redundant branches in the scenegraph since it would be very
 *   expensive for objects inserted near the end of the display list.
 *   - Could we have some sideband linking in the display list to be able to
 *   only walk through commands relating to ancestors?
 *      - If transform pops had a back link to transform pushes then it would
 *        be possible to efficiently skip over commands relating to redundant
 *        branches of the display list.
 *
 */

 typedef struct
{
  float x, y, z, w;
  float r, g, b, a;
} RigVertex4C4;

struct _RigGraph
{
  RigObjectProps _parent;
  int ref_count;

  RigGraphableProps graphable;
};

struct _RigNineSlice
{
  RigObjectProps _parent;
  int ref_count;

  CoglTexture *texture;

  float left;
  float right;
  float top;
  float bottom;

  float width;
  float height;

  CoglPipeline *pipeline;
  CoglPrimitive *primitive;

  RigGraphableProps graphable;
  RigPaintableProps paintable;

};

typedef enum _ButtonState
{
  BUTTON_STATE_NORMAL,
  BUTTON_STATE_HOVER,
  BUTTON_STATE_ACTIVE,
  BUTTON_STATE_ACTIVE_CANCEL,
  BUTTON_STATE_DISABLED
} ButtonState;

struct _RigButton
{
  RigObjectProps _parent;
  int ref_count;

  RigContext *ctx;

  ButtonState state;

  PangoLayout *label;
  int label_width;
  int label_height;

  float width;
  float height;

  RigNineSlice *background_normal;
  RigNineSlice *background_hover;
  RigNineSlice *background_active;
  RigNineSlice *background_disabled;

  CoglColor text_color;

  RigInputRegion *input_region;

  void (*on_click) (RigButton *button, void *user_data);
  void *on_click_data;

  RigSimpleWidgetProps simple_widget;

  RigGraphableProps graphable;
  RigPaintableProps paintable;
};

struct _RigText
{
  RigObjectProps _parent;
  int ref_count;

  RigContext *ctx;

  CoglBool enabled;
  CoglBool editable;

  PangoLayout *text;
  int text_width;
  int text_height;

  float width;
  float height;

  CoglColor text_color;

  RigInputRegion *input_region;

  RigGraphableProps graphable;
  RigPaintableProps paintable;
};

#define RIG_TOGGLE_BOX_WIDTH 15

enum {
  RIG_TOGGLE_PROP_STATE,
  RIG_TOGGLE_PROP_ENABLED,
  RIG_TOGGLE_N_PROPS
};

struct _RigToggle
{
  RigObjectProps _parent;
  int ref_count;

  RigContext *ctx;

  CoglBool state;
  CoglBool enabled;

  /* While we have the input grabbed we want to reflect what
   * the state will be when the mouse button is released
   * without actually changing the state... */
  CoglBool tentative_set;

  /* FIXME: we don't need a separate tick for every toggle! */
  PangoLayout *tick;

  PangoLayout *label;
  int label_width;
  int label_height;

  float width;
  float height;

  /* FIXME: we should be able to share border/box pipelines
   * between different toggle boxes. */
  CoglPipeline *pipeline_border;
  CoglPipeline *pipeline_box;

  CoglColor text_color;

  RigInputRegion *input_region;

  void (*on_toggle) (RigToggle *button, CoglBool value, void *user_data);
  void *on_toggle_data;

  RigGraphableProps graphable;
  RigPaintableProps paintable;

  RigSimpleIntrospectableProps introspectable;
  RigProperty properties[RIG_TOGGLE_N_PROPS];
};

static RigPropertySpec _rig_toggle_prop_specs[] = {
  {
    .name = "state",
    .type = RIG_PROPERTY_TYPE_BOOLEAN,
    .data_offset = offsetof (RigToggle, state),
    .setter = rig_toggle_set_state
  },
  {
    .name = "enabled",
    .type = RIG_PROPERTY_TYPE_BOOLEAN,
    .data_offset = offsetof (RigToggle, state),
    .setter = rig_toggle_set_enabled
  },
  { 0 } /* XXX: Needed for runtime counting of the number of properties */
};

struct _RigRectangle
{
  RigObjectProps _parent;
  int ref_count;

  float width;
  float height;

  RigSimpleWidgetProps simple_widget;

  RigGraphableProps graphable;
  RigPaintableProps paintable;

  CoglPipeline *pipeline;

};

typedef struct _RigTextureCacheEntry
{
  RigContext *ctx;
  char *filename;
  CoglTexture *texture;
} RigTextureCacheEntry;
#define RIG_TEXTURE_CACHE_ENTRY(X) ((RigTextureCacheEntry *)X)

static CoglUserDataKey texture_cache_key;

static uint8_t _rig_nine_slice_indices_data[] = {
    0,4,5,   0,5,1,  1,5,6,   1,6,2,   2,6,7,    2,7,3,
    4,8,9,   4,9,5,  5,9,10,  5,10,6,  6,10,11,  6,11,7,
    8,12,13, 8,13,9, 9,13,14, 9,14,10, 10,14,15, 10,15,11
};

typedef struct _SettingsChangedCallbackState
{
  RigSettingsChangedCallback callback;
  GDestroyNotify destroy_notify;
  void *user_data;
} SettingsChangedCallbackState;

struct _RigSettings
{
  GList *changed_callbacks;
};

static CoglUserDataKey fb_camera_key;

static void
_rig_settings_free (RigSettings *settings)
{
  GList *l;

  for (l = settings->changed_callbacks; l; l = l->next)
    g_slice_free (SettingsChangedCallbackState, l->data);
  g_list_free (settings->changed_callbacks);
}

RigSettings *
rig_settings_new (void)
{
  return g_slice_new0 (RigSettings);
}

void
rig_settings_add_changed_callback (RigSettings *settings,
                                   RigSettingsChangedCallback callback,
                                   GDestroyNotify destroy_notify,
                                   void *user_data)
{
  GList *l;
  SettingsChangedCallbackState *state;

  for (l = settings->changed_callbacks; l; l = l->next)
    {
      state = l->data;

      if (state->callback == callback)
        {
          state->user_data = user_data;
          state->destroy_notify = destroy_notify;
          return;
        }
    }

  state = g_slice_new (SettingsChangedCallbackState);
  state->callback = callback;
  state->destroy_notify = destroy_notify;
  state->user_data = user_data;

  settings->changed_callbacks = g_list_prepend (settings->changed_callbacks,
                                                state);
}

void
rig_settings_remove_changed_callback (RigSettings *settings,
                                      RigSettingsChangedCallback callback)
{
  GList *l;

  for (l = settings->changed_callbacks; l; l = l->next)
    {
      SettingsChangedCallbackState *state = l->data;

      if (state->callback == callback)
        {
          settings->changed_callbacks =
            g_list_delete_link (settings->changed_callbacks, l);
          g_slice_free (SettingsChangedCallbackState, state);
          return;
        }
    }
}

/* FIXME HACK */
unsigned int
rig_settings_get_password_hint_time (RigSettings *settings)
{
  return 10;
}

char *
rig_settings_get_font_name (RigSettings *settings)
{
  return g_strdup ("Sans 12");
}

static void
_rig_context_free (void *object)
{
  RigContext *ctx = object;

  rig_property_context_destroy (&ctx->property_ctx);

  g_object_unref (ctx->pango_context);
  g_object_unref (ctx->pango_font_map);
  pango_font_description_free (ctx->pango_font_desc);

  g_hash_table_destroy (ctx->texture_cache);

  if (rig_cogl_context == ctx->cogl_context)
    {
      cogl_object_unref (rig_cogl_context);
      rig_cogl_context = NULL;
    }

  cogl_object_unref (ctx->cogl_context);

  _rig_settings_free (ctx->settings);

  g_slice_free (RigContext, ctx);
}

static RigRefCountableVTable _rig_context_ref_countable_vtable = {
  rig_ref_countable_simple_ref,
  rig_ref_countable_simple_unref,
  _rig_context_free
};

RigType rig_context_type;

static void
_rig_context_init_type (void)
{
  rig_type_init (&rig_context_type);
  rig_type_add_interface (&rig_context_type,
                          RIG_INTERFACE_ID_REF_COUNTABLE,
                          offsetof (RigContext, ref_count),
                          &_rig_context_ref_countable_vtable);
}

static void
_rig_texture_cache_entry_free (RigTextureCacheEntry *entry)
{
  g_free (entry->filename);
  g_slice_free (RigTextureCacheEntry, entry);
}

static void
_rig_texture_cache_entry_destroy_cb (void *entry)
{
  _rig_texture_cache_entry_free (entry);
}

static void
texture_destroyed_cb (void *user_data)
{
  RigTextureCacheEntry *entry = user_data;

  g_hash_table_remove (entry->ctx->texture_cache, entry->filename);
  _rig_texture_cache_entry_free (entry);
}

CoglTexture *
rig_load_texture (RigContext *ctx, const char *filename, GError **error)
{
  RigTextureCacheEntry *entry =
    g_hash_table_lookup (ctx->texture_cache, filename);
  CoglTexture *texture;

  if (entry)
    return cogl_object_ref (entry->texture);

  texture = cogl_texture_new_from_file (filename,
                                        COGL_TEXTURE_NO_SLICING,
                                        COGL_PIXEL_FORMAT_ANY,
                                        error);
  if (!texture)
    return NULL;

  entry = g_slice_new0 (RigTextureCacheEntry);
  entry->ctx = ctx;
  entry->filename = g_strdup (filename);
  /* Note: we don't take a reference on the texture. The aim of this
   * cache is simply to avoid multiple loads of the same file and
   * doesn't affect the lifetime of the tracked textures. */
  entry->texture = texture;

  /* Track when the texture is freed... */
  cogl_object_set_user_data (COGL_OBJECT (texture),
                             &texture_cache_key,
                             entry,
                             texture_destroyed_cb);

  return texture;
}

RigContext *
rig_context_new (RigShell *shell)
{
  RigContext *context = g_new0 (RigContext, 1);
  GError *error = NULL;

  _rig_init ();

  rig_object_init (&context->_parent, &rig_context_type);

  context->ref_count = 1;

#ifdef USE_SDL
  context->cogl_context = cogl_sdl_context_new (SDL_USEREVENT, &error);
#else
  context->cogl_context = cogl_context_new (NULL, &error);
#endif
  if (!context->cogl_context)
    {
      g_warning ("Failed to create Cogl Context: %s", error->message);
      g_free (context);
      return NULL;
    }

  /* We set up the first created RigContext as a global default context */
  if (rig_cogl_context == NULL)
    rig_cogl_context = cogl_object_ref (context->cogl_context);

  context->settings = rig_settings_new ();

  context->texture_cache =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           NULL,
                           _rig_texture_cache_entry_destroy_cb);

  context->nine_slice_indices =
    cogl_indices_new (context->cogl_context,
                      COGL_INDICES_TYPE_UNSIGNED_BYTE,
                      _rig_nine_slice_indices_data,
                      sizeof (_rig_nine_slice_indices_data) /
                      sizeof (_rig_nine_slice_indices_data[0]));

  cogl_matrix_init_identity (&context->identity_matrix);

  context->pango_font_map =
    COGL_PANGO_FONT_MAP (cogl_pango_font_map_new (context->cogl_context));

  cogl_pango_font_map_set_use_mipmapping (context->pango_font_map, TRUE);

  context->pango_context =
    pango_font_map_create_context (PANGO_FONT_MAP (context->pango_font_map));

  context->pango_font_desc = pango_font_description_new ();
  pango_font_description_set_family (context->pango_font_desc, "Sans");
  pango_font_description_set_size (context->pango_font_desc, 14 * PANGO_SCALE);

  rig_property_context_init (&context->property_ctx);

  if (shell)
    {
      context->shell = rig_ref_countable_ref (shell);

      _rig_shell_associate_context (shell, context);
    }

  return context;
}

void
rig_context_init (RigContext *context)
{
  if (context->shell)
    _rig_shell_init (context->shell);
}

static void
_rig_camera_free (void *object)
{
  RigCamera *camera = object;
  GList *l;

  cogl_object_unref (camera->fb);

  for (l = camera->input_regions; l; l = l->next)
    rig_ref_countable_unref (l->data);
  g_list_free (camera->input_regions);

  g_slice_free (RigCamera, object);
}

RigRefCountableVTable _rig_camera_ref_countable = {
  rig_ref_countable_simple_ref,
  rig_ref_countable_simple_unref,
  _rig_camera_free
};

#if 0
static RigGraphableVTable _rig_camera_graphable_vtable = {
  NULL, /* child remove */
  NULL, /* child add */
  NULL /* parent changed */
};

static RigTransformableVTable _rig_camera_transformable_vtable = {
  rig_camera_get_view_transform
};
#endif

static void
draw_frustum (RigCamera *camera,
              CoglFramebuffer *fb)
{
  RigVertex4C4 vertices[8] = {
    /* near plane in projection space */
    {-1, -1, -1, 1, /* position */ .8, .8, .8, .8 /* color */ },
    { 1, -1, -1, 1,                .8, .8, .8, .8             },
    { 1,  1, -1, 1,                .8, .8, .8, .8             },
    {-1,  1, -1, 1,                .8, .8, .8, .8             },
    /* far plane in projection space */
    {-1, -1, 1, 1,  /* position */ .3, .3, .3, .3 /* color */ },
    { 1, -1, 1, 1,                 .3, .3, .3, .3             },
    { 1,  1, 1, 1,                 .3, .3, .3, .3             },
    {-1,  1, 1, 1,                 .3, .3, .3, .3             }
  };
  const CoglMatrix *projection_inv;
  CoglAttributeBuffer *attribute_buffer;
  CoglAttribute *attributes[2];
  CoglPrimitive *primitive;
  CoglPipeline *pipeline;
  CoglIndices *indices;
  unsigned char indices_data[24] = {
      0,1, 1,2, 2,3, 3,0,
      4,5, 5,6, 6,7, 7,4,
      0,4, 1,5, 2,6, 3,7
  };
  CoglDepthState depth_state;
  int i;

  projection_inv = rig_camera_get_inverse_projection (camera);

  for (i = 0; i < 8; i++)
    {
      cogl_matrix_transform_point (projection_inv,
                                   &vertices[i].x,
                                   &vertices[i].y,
                                   &vertices[i].z,
                                   &vertices[i].w);
      vertices[i].x /= vertices[i].w;
      vertices[i].y /= vertices[i].w;
      vertices[i].z /= vertices[i].w;
      vertices[i].w /= 1.0f;
    }

  attribute_buffer = cogl_attribute_buffer_new (rig_cogl_context,
                                                8 * sizeof (RigVertex4C4),
                                                vertices);

  attributes[0] = cogl_attribute_new (attribute_buffer,
                                      "cogl_position_in",
                                      sizeof (RigVertex4C4),
                                      offsetof (RigVertex4C4, x),
                                      3,
                                      COGL_ATTRIBUTE_TYPE_FLOAT);

  attributes[1] = cogl_attribute_new (attribute_buffer,
                                      "cogl_color_in",
                                      sizeof (RigVertex4C4),
                                      offsetof (RigVertex4C4, r),
                                      4,
                                      COGL_ATTRIBUTE_TYPE_FLOAT);

  indices = cogl_indices_new (rig_cogl_context,
                              COGL_INDICES_TYPE_UNSIGNED_BYTE,
                              indices_data,
                              G_N_ELEMENTS (indices_data));

  primitive = cogl_primitive_new_with_attributes (COGL_VERTICES_MODE_LINES,
                                                  8, attributes, 2);

  cogl_primitive_set_indices (primitive, indices, G_N_ELEMENTS(indices_data));

  cogl_object_unref (attribute_buffer);
  cogl_object_unref (attributes[0]);
  cogl_object_unref (attributes[1]);
  cogl_object_unref (indices);

  pipeline = cogl_pipeline_new (rig_cogl_context);

  /* enable depth testing */
  cogl_depth_state_init (&depth_state);
  cogl_depth_state_set_test_enabled (&depth_state, TRUE);
  cogl_pipeline_set_depth_state (pipeline, &depth_state, NULL);

  cogl_framebuffer_draw_primitive (fb, pipeline, primitive);

  cogl_object_unref (primitive);
  cogl_object_unref (pipeline);
}

typedef struct _CameraFlushState
{
  RigCamera *current_camera;
  unsigned int transform_age;
  CoglBool in_frame;
} CameraFlushState;

static void
free_camera_flush_state (void *user_data)
{
  CameraFlushState *state = user_data;
  g_slice_free (CameraFlushState, state);
}

static void
_rig_camera_flush_transforms (RigCamera *camera)
{
  const CoglMatrix *projection;
  CoglFramebuffer *fb = camera->fb;
  CameraFlushState *state =
    cogl_object_get_user_data (COGL_OBJECT (fb), &fb_camera_key);
  if (!state)
    {
      state = g_slice_new (CameraFlushState);
      state->in_frame = FALSE;
      cogl_object_set_user_data (COGL_OBJECT (fb),
                                 &fb_camera_key,
                                 state,
                                 free_camera_flush_state);
    }
  else if (state->current_camera == camera &&
           camera->transform_age == state->transform_age)
    {
      state->in_frame = TRUE;
      return;
    }

  if (state->in_frame)
    {
      g_warning ("Un-balanced rig_camera_flush/_end calls: "
                 "repeat _flush() calls before _end()");
    }

  cogl_framebuffer_set_viewport (fb,
                                 camera->viewport[0],
                                 camera->viewport[1],
                                 camera->viewport[2],
                                 camera->viewport[3]);

  projection = rig_camera_get_projection (camera);
  cogl_framebuffer_set_projection_matrix (fb, projection);

  cogl_framebuffer_set_modelview_matrix (fb, &camera->view);

  state->current_camera = camera;
  state->transform_age = camera->transform_age;
  state->in_frame = TRUE;
}

void
rig_camera_draw (RigObject *object,
                 CoglFramebuffer *fb)
{
  RigCamera *camera = object;

  if (fb != camera->fb)
    {
      /* When the fb we have to draw to is not the one this camera is
       * responsible for, we can draw its frustum to visualize it */
      draw_frustum (camera, fb);
      return;
    }

  _rig_camera_flush_transforms (camera);

  if (camera->clear_fb)
    {
      cogl_framebuffer_clear4f (fb,
                                COGL_BUFFER_BIT_COLOR |
                                COGL_BUFFER_BIT_DEPTH |
                                COGL_BUFFER_BIT_STENCIL,
                                cogl_color_get_red_float (&camera->bg_color),
                                cogl_color_get_green_float (&camera->bg_color),
                                cogl_color_get_blue_float (&camera->bg_color),
                                cogl_color_get_alpha_float (&camera->bg_color));
    }
}

static RigComponentableVTable _rig_camera_componentable_vtable = {
  .draw = rig_camera_draw
};

RigType rig_camera_type;

static void
_rig_camera_init_type (void)
{
  rig_type_init (&rig_camera_type);
  rig_type_add_interface (&rig_camera_type,
                           RIG_INTERFACE_ID_REF_COUNTABLE,
                           offsetof (RigCamera, ref_count),
                           &_rig_camera_ref_countable);
  rig_type_add_interface (&rig_camera_type,
                           RIG_INTERFACE_ID_COMPONENTABLE,
                           offsetof (RigCamera, component),
                           &_rig_camera_componentable_vtable);

#if 0
  rig_type_add_interface (&rig_camera_type,
                           RIG_INTERFACE_ID_GRAPHABLE,
                           offsetof (RigCamera, graphable),
                           &_rig_camera_graphable_vtable);
  rig_type_add_interface (&rig_camera_type,
                           RIG_INTERFACE_ID_TRANSFORMABLE,
                           0,
                           &_rig_camera_transformable_vtable);
#endif
}

RigCamera *
rig_camera_new (RigContext *ctx, CoglFramebuffer *framebuffer)
{
  RigCamera *camera = g_slice_new0 (RigCamera);
  int width = cogl_framebuffer_get_width (framebuffer);
  int height = cogl_framebuffer_get_height (framebuffer);

  camera->ctx = rig_ref_countable_ref (ctx);

  rig_object_init (&camera->_parent, &rig_camera_type);

  camera->ref_count = 1;

  camera->component.type = RIG_COMPONENT_TYPE_CAMERA;

  rig_camera_set_background_color4f (camera, 0, 0, 0, 1);
  camera->clear_fb = TRUE;

  //rig_graphable_init (RIG_OBJECT (camera));

  camera->orthographic = TRUE;
  camera->x1 = 0;
  camera->y1 = 0;
  camera->x2 = width;
  camera->y2 = height;
  camera->near = -1;
  camera->far = 100;

  camera->projection_cache_age = -1;
  camera->inverse_projection_age = -1;

  cogl_matrix_init_identity (&camera->view);
  camera->inverse_view_age = -1;

  camera->viewport[2] = width;
  camera->viewport[3] = height;

  camera->transform_age = 0;

  cogl_matrix_init_identity (&camera->input_transform);

  camera->fb = cogl_object_ref (framebuffer);

  camera->frame = 0;
  camera->timer = g_timer_new ();
  g_timer_start (camera->timer);

  return camera;
}

void
rig_camera_set_background_color4f (RigCamera *camera,
                                   float red,
                                   float green,
                                   float blue,
                                   float alpha)
{
  cogl_color_init_from_4f (&camera->bg_color,
                           red, green, blue, alpha);
}

void
rig_camera_set_clear (RigCamera *camera,
                      CoglBool clear)
{
  if (clear)
    camera->clear_fb = TRUE;
  else
    camera->clear_fb = FALSE;
}

CoglFramebuffer *
rig_camera_get_framebuffer (RigCamera *camera)
{
  return camera->fb;
}

void
rig_camera_set_framebuffer (RigCamera *camera,
                            CoglFramebuffer *framebuffer)
{
  if (camera->fb == framebuffer)
    return;

  cogl_object_unref (camera->fb);
  camera->fb = cogl_object_ref (camera->fb);
}

void
rig_camera_set_viewport (RigCamera *camera,
                         float x,
                         float y,
                         float width,
                         float height)
{
  if (camera->viewport[0] == x &&
      camera->viewport[1] == y &&
      camera->viewport[2] == width &&
      camera->viewport[3] == height)
    return;

  camera->viewport[0] = x;
  camera->viewport[1] = y;
  camera->viewport[2] = width;
  camera->viewport[3] = height;

  camera->transform_age++;
}

const float *
rig_camera_get_viewport (RigCamera *camera)
{
  return camera->viewport;
}

const CoglMatrix *
rig_camera_get_projection (RigCamera *camera)
{
  if (G_UNLIKELY (camera->projection_cache_age != camera->projection_age))
    {
      cogl_matrix_init_identity (&camera->projection);

      if (camera->orthographic)
        {
          cogl_matrix_orthographic (&camera->projection,
                                    camera->x1,
                                    camera->y1,
                                    camera->x2,
                                    camera->y2,
                                    camera->near,
                                    camera->far);
        }
      else
        {
          float aspect_ratio = camera->viewport[2] / camera->viewport[3];
          cogl_matrix_perspective (&camera->projection,
                                   camera->fov,
                                   aspect_ratio,
                                   camera->near,
                                   camera->far);
        }

      camera->projection_cache_age = camera->projection_age;
    }

  return &camera->projection;
}

void
rig_camera_set_near_plane (RigCamera *camera,
                           float near)
{
  camera->near = near;
  camera->projection_age++;
  camera->transform_age++;
}

float
rig_camera_get_near_plane (RigCamera *camera)
{
  return camera->near;
}

void
rig_camera_set_far_plane (RigCamera *camera,
                          float far)
{
  camera->far = far;
  camera->projection_age++;
  camera->transform_age++;
}

float
rig_camera_get_far_plane (RigCamera *camera)
{
  return camera->far;
}

RigProjection
rig_camera_get_projection_mode (RigCamera *camera)
{
  if (camera->orthographic)
    return RIG_PROJECTION_ORTHOGRAPHIC;
  else
    return RIG_PROJECTION_PERSPECTIVE;
}

void
rig_camera_set_projection_mode (RigCamera  *camera,
                                RigProjection projection)
{
  CoglBool orthographic;

  if (projection == RIG_PROJECTION_ORTHOGRAPHIC)
    orthographic = TRUE;
  else
    orthographic = FALSE;

  if (orthographic != camera->orthographic)
    {
      camera->orthographic = orthographic;
      camera->projection_age++;
      camera->transform_age++;
    }
}

void
rig_camera_set_field_of_view (RigCamera *camera,
                              float fov)
{
  if (camera->fov == fov)
    return;

  camera->fov = fov;
  if (!camera->orthographic)
    {
      camera->projection_age++;
      camera->transform_age++;
    }
}

float
rig_camera_get_field_of_view (RigCamera *camera)
{
  return camera->fov;
}

void
rig_camera_set_orthographic_coordinates (RigCamera *camera,
                                         float x1,
                                         float y1,
                                         float x2,
                                         float y2)
{
  if (camera->x1 == x1 &&
      camera->y1 == y1 &&
      camera->x2 == x2 &&
      camera->y2 == y2)
    return;

  camera->x1 = x1;
  camera->y1 = y1;
  camera->x2 = x2;
  camera->y2 = y2;

  if (camera->orthographic)
    camera->projection_age++;
}

const CoglMatrix *
rig_camera_get_inverse_projection (RigCamera *camera)
{
  if (camera->inverse_projection_age == camera->projection_age)
    return &camera->inverse_projection;

  if (!cogl_matrix_get_inverse (&camera->projection,
                                &camera->inverse_projection))
    return NULL;

  camera->inverse_projection_age = camera->projection_age;
  return &camera->inverse_projection;
}

void
rig_camera_set_view_transform (RigCamera *camera,
                               const CoglMatrix *view)
{
  camera->view = *view;

  camera->view_age++;
  camera->transform_age++;

  /* XXX: we have no way to assert that we are at the bottom of the
   * matrix stack at this point, so this might do bad things...
   */
  //cogl_framebuffer_set_modelview_matrix (camera->fb,
  //                                       &camera->view);
}

const CoglMatrix *
rig_camera_get_view_transform (RigCamera *camera)
{
  return &camera->view;
}

const CoglMatrix *
rig_camera_get_inverse_view_transform (RigCamera *camera)
{
  if (camera->inverse_view_age == camera->view_age)
    return &camera->inverse_view;

  if (!cogl_matrix_get_inverse (&camera->view,
                                &camera->inverse_view))
    return NULL;

  camera->inverse_view_age = camera->view_age;
  return &camera->inverse_view;
}

void
rig_camera_set_input_transform (RigCamera *camera,
                                const CoglMatrix *input_transform)
{
  camera->input_transform = *input_transform;
}

void
rig_camera_add_input_region (RigCamera *camera,
                             RigInputRegion *region)
{
  g_print ("add input region %p, %p\n", camera, region);
  camera->input_regions = g_list_prepend (camera->input_regions, region);
}

void
rig_camera_remove_input_region (RigCamera *camera,
                                RigInputRegion *region)
{
  camera->input_regions = g_list_remove (camera->input_regions, region);
}

#if 0
typedef struct _RigCameraInputCallbackState
{
  RigInputCallback callback;
  void *user_data;
} RigCameraInputCallbackState;

RigInputEventStatus
_rig_camera_input_callback_wrapper (RigCameraInputCallbackState *state,
                                    RigInputEvent *event)
{
  RigCamera *camera = state->camera;
  float *viewport = camera->viewport;

  cogl_matrix_translate (event->input_transform,
                         viewport[0], viewport[1], 0);
  cogl_matrix_scale (event->input_transform,
                     cogl_framebuffer_get_width (camera->fb) / viewport[2],
                     cogl_framebuffer_get_height (camera->fb) / viewport[3]);

  return state->callback (event, state->user_data);
}

void
rig_camera_add_input_callback (RigCamera *camera,
                               RigInputCallback callback,
                               void *user_data)
{
  RigCameraInputCallbackState *state = g_slice_new (RigCameraInputCallbackState);
  state->camera = camera;
  state->callback = callback;
  state->user_data = user_data;
  camera->input_callbacks = g_list_prepend (camera->input_callbacks, state);
}
#endif

CoglBool
rig_camera_transform_window_coordinate (RigCamera *camera,
                                        float *x,
                                        float *y)
{
  float *viewport = camera->viewport;
  *x -= viewport[0];
  *y -= viewport[1];

  if (*x < 0 || *x >= viewport[2] || *y < 0 || *y >= viewport[3])
    return FALSE;
  else
    return TRUE;
}

void
rig_camera_unproject_coord (RigCamera *camera,
                            const CoglMatrix *modelview,
                            const CoglMatrix *inverse_modelview,
                            float object_coord_z,
                            float *x,
                            float *y)
{
  const CoglMatrix *projection = rig_camera_get_projection (camera);
  const CoglMatrix *inverse_projection =
    rig_camera_get_inverse_projection (camera);
  //float z;
  float ndc_x, ndc_y, ndc_z, ndc_w;
  float eye_x, eye_y, eye_z, eye_w;
  const float *viewport = rig_camera_get_viewport (camera);

  /* Convert item z into NDC z */
  {
    //float x = 0, y = 0, z = 0, w = 1;
    float z = 0, w = 1;
    float tmp_x, tmp_y, tmp_z;
    const CoglMatrix *m = modelview;

    tmp_x = m->xw;
    tmp_y = m->yw;
    tmp_z = m->zw;

    m = projection;
    z = m->zx * tmp_x + m->zy * tmp_y + m->zz * tmp_z + m->zw;
    w = m->wx * tmp_x + m->wy * tmp_y + m->wz * tmp_z + m->ww;

    ndc_z = z / w;
  }

  /* Undo the Viewport transform, putting us in Normalized Device Coords */
  ndc_x = (*x - viewport[0]) * 2.0f / viewport[2] - 1.0f;
  ndc_y = ((viewport[3] - 1 + viewport[1] - *y) * 2.0f / viewport[3] - 1.0f);

  /* Undo the Projection, putting us in Eye Coords. */
  ndc_w = 1;
  cogl_matrix_transform_point (inverse_projection,
                               &ndc_x, &ndc_y, &ndc_z, &ndc_w);
  eye_x = ndc_x / ndc_w;
  eye_y = ndc_y / ndc_w;
  eye_z = ndc_z / ndc_w;
  eye_w = 1;

  /* Undo the Modelview transform, putting us in Object Coords */
  cogl_matrix_transform_point (inverse_modelview,
                               &eye_x,
                               &eye_y,
                               &eye_z,
                               &eye_w);

  *x = eye_x;
  *y = eye_y;
  //*z = eye_z;
}

void
rig_camera_flush (RigCamera *camera)
{
  rig_camera_draw (camera, camera->fb);
}

void
rig_camera_end_frame (RigCamera *camera)
{
  double elapsed;
  CameraFlushState *state =
    cogl_object_get_user_data (COGL_OBJECT (camera->fb), &fb_camera_key);
  if (state)
    {
      if (state->in_frame != TRUE)
        g_warning ("Un-balanced rig_camera_flush/end frame calls. "
                   "_end before _flush");
      state->in_frame = FALSE;
    }
  camera->frame++;

  elapsed = g_timer_elapsed (camera->timer, NULL);
  if (elapsed > 1.0)
    {
      g_print ("fps = %f (camera = %p)\n",
               camera->frame / elapsed,
               camera);
      g_timer_start (camera->timer);
      camera->frame = 0;
    }
}

void
rig_paintable_init (RigObject *object)
{
#if 0
  RigPaintableProps *props =
    rig_object_get_properties (object, RIG_INTERFACE_ID_PAINTABLE);
#endif
}

void
rig_paintable_paint (RigObject *object, RigPaintContext *paint_ctx)
{
  RigPaintableVTable *paintable = rig_object_get_vtable (object, RIG_INTERFACE_ID_PAINTABLE);

  paintable->paint (object, paint_ctx);
}

static void
_rig_nine_slice_free (void *object)
{
  RigNineSlice *nine_slice = object;

  cogl_object_unref (nine_slice->texture);

  cogl_object_unref (nine_slice->pipeline);
  cogl_object_unref (nine_slice->primitive);

  g_slice_free (RigNineSlice, object);
}

RigRefCountableVTable _rig_nine_slice_ref_countable_vtable = {
  rig_ref_countable_simple_ref,
  rig_ref_countable_simple_unref,
  _rig_nine_slice_free
};

static RigGraphableVTable _rig_nine_slice_graphable_vtable = {
  NULL, /* child remove */
  NULL, /* child add */
  NULL /* parent changed */
};

static void
_rig_nine_slice_paint (RigObject *object,
                       RigPaintContext *paint_ctx)
{
  RigNineSlice *nine_slice = RIG_NINE_SLICE (object);
  RigCamera *camera = paint_ctx->camera;

  cogl_framebuffer_draw_primitive (camera->fb,
                                   nine_slice->pipeline,
                                   nine_slice->primitive);
}

static RigPaintableVTable _rig_nine_slice_paintable_vtable = {
  _rig_nine_slice_paint
};

RigType rig_nine_slice_type;

static void
_rig_nine_slice_init_type (void)
{
  rig_type_init (&rig_nine_slice_type);
  rig_type_add_interface (&rig_nine_slice_type,
                           RIG_INTERFACE_ID_REF_COUNTABLE,
                           offsetof (RigNineSlice, ref_count),
                           &_rig_nine_slice_ref_countable_vtable);
  rig_type_add_interface (&rig_nine_slice_type,
                           RIG_INTERFACE_ID_GRAPHABLE,
                           offsetof (RigNineSlice, graphable),
                           &_rig_nine_slice_graphable_vtable);
  rig_type_add_interface (&rig_nine_slice_type,
                           RIG_INTERFACE_ID_PAINTABLE,
                           offsetof (RigNineSlice, paintable),
                           &_rig_nine_slice_paintable_vtable);
}

static CoglPrimitive *
primitive_new_textured_rectangle (RigContext *ctx,
                                  float x0,
                                  float y0,
                                  float x1,
                                  float y1,
                                  float s0,
                                  float t0,
                                  float s1,
                                  float t1)
{
  CoglVertexP2T2 vertices[8] = {
        {x0, y0, s0, t0},
        {x0, y1, s0, t1},
        {x1, y1, s1, t1},
        {x1, y0, s1, t0}
  };

  return cogl_primitive_new_p2t2 (ctx->cogl_context,
                                  COGL_VERTICES_MODE_TRIANGLE_STRIP,
                                  8,
                                  vertices);
}

static RigNineSlice *
_rig_nine_slice_new_full (RigContext *ctx,
                          CoglTexture *texture,
                          float top,
                          float right,
                          float bottom,
                          float left,
                          float width,
                          float height,
                          CoglPrimitive *shared_prim)
{
  RigNineSlice *nine_slice = g_slice_new (RigNineSlice);

  rig_object_init (&nine_slice->_parent, &rig_nine_slice_type);

  nine_slice->ref_count = 1;

  rig_graphable_init (RIG_OBJECT (nine_slice));

  nine_slice->texture = cogl_object_ref (texture);

  nine_slice->left = left;
  nine_slice->right = right;
  nine_slice->top = top;
  nine_slice->bottom = bottom;

  nine_slice->width = width;
  nine_slice->height = height;

  nine_slice->pipeline = cogl_pipeline_new (ctx->cogl_context);
  cogl_pipeline_set_layer_texture (nine_slice->pipeline, 0, texture);

  /* simple stretch */
  if (left == 0 && right == 0 && top == 0 && bottom == 0)
    {
      nine_slice->primitive =
        primitive_new_textured_rectangle (ctx,
                                          0, 0, width, height,
                                          0, 0, 1, 1);
    }
  else if (shared_prim)
    {
      nine_slice->primitive = cogl_object_ref (shared_prim);
    }
  else
    {
      float tex_width = cogl_texture_get_width (texture);
      float tex_height = cogl_texture_get_height (texture);

      /* x0,y0,x1,y1 and s0,t0,s1,t1 define the postion and texture
       * coordinates for the center rectangle... */
      float x0 = left;
      float y0 = top;
      float x1 = width - right;
      float y1 = height - bottom;

      float s0 = left / tex_width;
      float t0 = top / tex_height;
      float s1 = (tex_width - right) / tex_width;
      float t1 = (tex_height - bottom) / tex_height;

      /*
       * 0,0      x0,0      x1,0      width,0
       * 0,0      s0,0      s1,0      1,0
       * 0        1         2         3
       *
       * 0,y0     x0,y0     x1,y0     width,y0
       * 0,t0     s0,t0     s1,t0     1,t0
       * 4        5         6         7
       *
       * 0,y1     x0,y1     x1,y1     width,y1
       * 0,t1     s0,t1     s1,t1     1,t1
       * 8        9         10        11
       *
       * 0,height x0,height x1,height width,height
       * 0,1      s0,1      s1,1      1,1
       * 12       13        14        15
       */

      CoglVertexP2T2 vertices[] =
        {
            { 0, 0, 0, 0 },
            { x0, 0, s0, 0},
            { x1, 0, s1, 0},
            { width, 0, 1, 0},

            { 0, y0, 0, t0},
            { x0, y0, s0, t0},
            { x1, y0, s1, t0},
            { width, y0, 1, t0},

            { 0, y1, 0, t1},
            { x0, y1, s0, t1},
            { x1, y1, s1, t1},
            { width, y1, 1, t1},

            { 0, height, 0, 1},
            { x0, height, s0, 1},
            { x1, height, s1, 1},
            { width, height, 1, 1},
        };

      nine_slice->primitive =
        cogl_primitive_new_p2t2 (ctx->cogl_context,
                                 COGL_VERTICES_MODE_TRIANGLES,
                                 sizeof (vertices) / sizeof (CoglVertexP2T2),
                                 vertices);

      /* The vertices uploaded only map to the key intersection points of the
       * 9-slice grid which isn't a topology that GPUs can handle directly so
       * this specifies an array of indices that allow the GPU to interpret the
       * vertices as a list of triangles... */
      cogl_primitive_set_indices (nine_slice->primitive,
                                  ctx->nine_slice_indices,
                                  sizeof (_rig_nine_slice_indices_data) /
                                  sizeof (_rig_nine_slice_indices_data[0]));
    }

  return nine_slice;
}

RigNineSlice *
rig_nine_slice_new (RigContext *ctx,
                    CoglTexture *texture,
                    float top,
                    float right,
                    float bottom,
                    float left,
                    float width,
                    float height)
{
  return _rig_nine_slice_new_full (ctx, texture,
                                   top, right, bottom, left,
                                   width, height,
                                   NULL);
}

static void
_rig_graph_free (void *object)
{
  RigGraph *graph = RIG_GRAPH (object);

  rig_graphable_remove_all_children (graph);

  g_slice_free (RigGraph, object);
}

RigRefCountableVTable _rig_graph_ref_countable_vtable = {
  rig_ref_countable_simple_ref,
  rig_ref_countable_simple_unref,
  _rig_graph_free
};

RigGraphableVTable _rig_graph_graphable_vtable = {
  NULL, /* child remove */
  NULL, /* child add */
  NULL /* parent changed */
};

RigType rig_graph_type;

static void
_rig_graph_init_type (void)
{
  rig_type_init (&rig_graph_type);
  rig_type_add_interface (&rig_graph_type,
                           RIG_INTERFACE_ID_REF_COUNTABLE,
                           offsetof (RigButton, ref_count),
                           &_rig_graph_ref_countable_vtable);
  rig_type_add_interface (&rig_graph_type,
                           RIG_INTERFACE_ID_GRAPHABLE,
                           offsetof (RigGraph, graphable),
                           &_rig_graph_graphable_vtable);
}

RigGraph *
rig_graph_new (RigContext *ctx, ...)
{
  RigGraph *graph = g_slice_new (RigGraph);
  RigObject *object;
  va_list ap;

  rig_object_init (&graph->_parent, &rig_graph_type);

  graph->ref_count = 1;

  rig_graphable_init (RIG_OBJECT (graph));

  va_start (ap, ctx);
  while ((object = va_arg (ap, RigObject *)))
    rig_graphable_add_child (RIG_OBJECT (graph), object);
  va_end (ap);

  return graph;
}

static void
_rig_transform_free (void *object)
{
  RigTransform *transform = RIG_TRANSFORM (object);

  rig_graphable_remove_all_children (transform);

  g_slice_free (RigTransform, object);
}

RigRefCountableVTable _rig_transform_ref_countable_vtable = {
  rig_ref_countable_simple_ref,
  rig_ref_countable_simple_unref,
  _rig_transform_free
};

static RigGraphableVTable _rig_transform_graphable_vtable = {
  NULL, /* child remove */
  NULL, /* child add */
  NULL /* parent changed */
};

static RigTransformableVTable _rig_transform_transformable_vtable = {
  rig_transform_get_matrix
};

RigType rig_transform_type;

static void
_rig_transform_init_type (void)
{
  rig_type_init (&rig_transform_type);
  rig_type_add_interface (&rig_transform_type,
                           RIG_INTERFACE_ID_REF_COUNTABLE,
                           offsetof (RigButton, ref_count),
                           &_rig_transform_ref_countable_vtable);
  rig_type_add_interface (&rig_transform_type,
                           RIG_INTERFACE_ID_GRAPHABLE,
                           offsetof (RigTransform, graphable),
                           &_rig_transform_graphable_vtable);
  rig_type_add_interface (&rig_transform_type,
                           RIG_INTERFACE_ID_TRANSFORMABLE,
                           0,
                           &_rig_transform_transformable_vtable);
}

RigTransform *
rig_transform_new (RigContext *ctx,
                   ...)
{
  RigTransform *transform = g_slice_new (RigTransform);
  RigObject *object;
  va_list ap;

  rig_object_init (&transform->_parent, &rig_transform_type);

  transform->ref_count = 1;

  rig_graphable_init (RIG_OBJECT (transform));

  cogl_matrix_init_identity (&transform->matrix);

  va_start (ap, ctx);
  while ((object = va_arg (ap, RigObject *)))
    rig_graphable_add_child (RIG_OBJECT (transform), object);
  va_end (ap);

  return transform;
}

void
rig_transform_translate (RigTransform *transform,
                         float x,
                         float y,
                         float z)
{
  cogl_matrix_translate (&transform->matrix, x, y, z);
}

void
rig_transform_quaternion_rotate (RigTransform *transform,
                                 const CoglQuaternion *quaternion)
{
  CoglMatrix rotation;
  cogl_matrix_init_from_quaternion (&rotation, quaternion);
  cogl_matrix_multiply (&transform->matrix, &transform->matrix, &rotation);
}

void
rig_transform_rotate (RigTransform *transform,
                      float angle,
                      float x,
                      float y,
                      float z)
{
  cogl_matrix_rotate (&transform->matrix, angle, x, y, z);
}
void
rig_transform_scale (RigTransform *transform,
                     float x,
                     float y,
                     float z)
{
  cogl_matrix_scale (&transform->matrix, x, y, z);
}

void
rig_transform_init_identity (RigTransform *transform)
{
  cogl_matrix_init_identity (&transform->matrix);
}

const CoglMatrix *
rig_transform_get_matrix (RigTransform *transform)
{
  return &transform->matrix;
}

void
rig_simple_widget_graphable_parent_changed (RigObject *self,
                                            RigObject *old_parent,
                                            RigObject *new_parent)
{
  /* nop */
}

void
rig_simple_widget_graphable_child_removed_warn (RigObject *self,
                                                RigObject *child)
{
  /* You can't add children to a button currently */
  g_warn_if_reached ();
}

void
rig_simple_widget_graphable_child_added_warn (RigObject *self,
                                              RigObject *child)
{
  /* You can't add children to a button currently */
  g_warn_if_reached ();
}

static void
_rig_rectangle_free (void *object)
{
  RigRectangle *rectangle = object;

  cogl_object_unref (rectangle->pipeline);

  g_slice_free (RigRectangle, object);
}

static RigRefCountableVTable _rig_rectangle_ref_countable_vtable = {
  rig_ref_countable_simple_ref,
  rig_ref_countable_simple_unref,
  _rig_rectangle_free
};

static RigGraphableVTable _rig_rectangle_graphable_vtable = {
  NULL, /* child remove */
  NULL, /* child add */
  NULL /* parent changed */
};

static void
_rig_rectangle_paint (RigObject *object,
                      RigPaintContext *paint_ctx)
{
  RigRectangle *rectangle = RIG_RECTANGLE (object);
  RigCamera *camera = paint_ctx->camera;

  cogl_framebuffer_draw_rectangle (camera->fb,
                                   rectangle->pipeline,
                                   0, 0,
                                   rectangle->width,
                                   rectangle->height);
}

static RigPaintableVTable _rig_rectangle_paintable_vtable = {
  _rig_rectangle_paint
};

static RigSimpleWidgetVTable _rig_rectangle_simple_widget_vtable = {
 0
};

RigType rig_rectangle_type;

static void
_rig_rectangle_init_type (void)
{
  rig_type_init (&rig_rectangle_type);
  rig_type_add_interface (&rig_rectangle_type,
                          RIG_INTERFACE_ID_REF_COUNTABLE,
                          offsetof (RigRectangle, ref_count),
                          &_rig_rectangle_ref_countable_vtable);
  rig_type_add_interface (&rig_rectangle_type,
                          RIG_INTERFACE_ID_GRAPHABLE,
                          offsetof (RigRectangle, graphable),
                          &_rig_rectangle_graphable_vtable);
  rig_type_add_interface (&rig_rectangle_type,
                          RIG_INTERFACE_ID_PAINTABLE,
                          offsetof (RigRectangle, paintable),
                          &_rig_rectangle_paintable_vtable);
  rig_type_add_interface (&rig_rectangle_type,
                          RIG_INTERFACE_ID_SIMPLE_WIDGET,
                          offsetof (RigRectangle, simple_widget),
                          &_rig_rectangle_simple_widget_vtable);
}

RigRectangle *
rig_rectangle_new4f (RigContext *ctx,
                     float width,
                     float height,
                     float red,
                     float green,
                     float blue,
                     float alpha)
{
  RigRectangle *rectangle = g_slice_new0 (RigRectangle);

  rig_object_init (RIG_OBJECT (rectangle), &rig_rectangle_type);

  rectangle->ref_count = 1;

  rig_graphable_init (RIG_OBJECT (rectangle));
  rig_paintable_init (RIG_OBJECT (rectangle));

  rectangle->width = width;
  rectangle->height = height;

  rectangle->pipeline = cogl_pipeline_new (ctx->cogl_context);
  cogl_pipeline_set_color4f (rectangle->pipeline,
                             red, green, blue, alpha);

  return rectangle;
}

void
rig_rectangle_set_width (RigRectangle *rectangle, float width)
{
  rectangle->width = width;
}

void
rig_rectangle_set_height (RigRectangle *rectangle, float height)
{
  rectangle->height = height;
}

static void
_rig_toggle_free (void *object)
{
  RigToggle *toggle = object;

  g_object_unref (toggle->tick);
  g_object_unref (toggle->label);

  cogl_object_unref (toggle->pipeline_border);
  cogl_object_unref (toggle->pipeline_box);

  rig_simple_introspectable_destroy (toggle);

  g_slice_free (RigToggle, object);
}

RigRefCountableVTable _rig_toggle_ref_countable_vtable = {
  rig_ref_countable_simple_ref,
  rig_ref_countable_simple_unref,
  _rig_toggle_free
};

static RigGraphableVTable _rig_toggle_graphable_vtable = {
  NULL, /* child remove */
  NULL, /* child add */
  NULL /* parent changed */
};

static void
_rig_toggle_paint (RigObject *object,
                   RigPaintContext *paint_ctx)
{
  RigToggle *toggle = RIG_TOGGLE (object);
  RigCamera *camera = paint_ctx->camera;
  CoglFramebuffer *fb = camera->fb;
  int box_y;

  /* FIXME: This is a fairly lame way of drawing a check box! */

  box_y = (toggle->label_height / 2.0) - (RIG_TOGGLE_BOX_WIDTH / 2.0);


  cogl_framebuffer_draw_rectangle (fb,
                                   toggle->pipeline_border,
                                   0, box_y,
                                   RIG_TOGGLE_BOX_WIDTH,
                                   box_y + RIG_TOGGLE_BOX_WIDTH);

  cogl_framebuffer_draw_rectangle (fb,
                                   toggle->pipeline_box,
                                   1, box_y + 1,
                                   RIG_TOGGLE_BOX_WIDTH - 2,
                                   box_y + RIG_TOGGLE_BOX_WIDTH - 2);

  if (toggle->state || toggle->tentative_set)
    cogl_pango_show_layout (camera->fb,
                            toggle->tick,
                            0, 0,
                            &toggle->text_color);

  cogl_pango_show_layout (camera->fb,
                          toggle->label,
                          RIG_TOGGLE_BOX_WIDTH + 5, 0,
                          &toggle->text_color);
}

static RigPaintableVTable _rig_toggle_paintable_vtable = {
  _rig_toggle_paint
};

static RigIntrospectableVTable _rig_toggle_introspectable_vtable = {
  rig_simple_introspectable_lookup_property,
  rig_simple_introspectable_foreach_property
};

RigSimpleWidgetVTable _rig_toggle_simple_widget_vtable = {
 0
};

RigType rig_toggle_type;

static void
_rig_toggle_init_type (void)
{
  rig_type_init (&rig_toggle_type);
  rig_type_add_interface (&rig_toggle_type,
                          RIG_INTERFACE_ID_REF_COUNTABLE,
                          offsetof (RigToggle, ref_count),
                          &_rig_toggle_ref_countable_vtable);
  rig_type_add_interface (&rig_toggle_type,
                          RIG_INTERFACE_ID_GRAPHABLE,
                          offsetof (RigToggle, graphable),
                          &_rig_toggle_graphable_vtable);
  rig_type_add_interface (&rig_toggle_type,
                          RIG_INTERFACE_ID_PAINTABLE,
                          offsetof (RigToggle, paintable),
                          &_rig_toggle_paintable_vtable);
  rig_type_add_interface (&rig_toggle_type,
                          RIG_INTERFACE_ID_INTROSPECTABLE,
                          0, /* no implied properties */
                          &_rig_toggle_introspectable_vtable);
  rig_type_add_interface (&rig_toggle_type,
                          RIG_INTERFACE_ID_SIMPLE_INTROSPECTABLE,
                          offsetof (RigToggle, introspectable),
                          NULL); /* no implied vtable */
}

typedef struct _ToggleGrabState
{
  RigCamera *camera;
  RigInputRegion *region;
  RigToggle *toggle;
} ToggleGrabState;

static RigInputEventStatus
_rig_toggle_grab_input_cb (RigInputEvent *event,
                           void *user_data)
{
  ToggleGrabState *state = user_data;
  RigToggle *toggle = state->toggle;

  if(rig_input_event_get_type (event) == RIG_INPUT_EVENT_TYPE_MOTION)
    {
      RigShell *shell = toggle->ctx->shell;
      if (rig_motion_event_get_action (event) == RIG_MOTION_EVENT_ACTION_UP)
        {
          float x = rig_motion_event_get_x (event);
          float y = rig_motion_event_get_y (event);

          rig_shell_ungrab_input (shell);

          if (rig_camera_pick_input_region (state->camera,
                                            state->region,
                                            x, y))
            {
              toggle->state = !toggle->state;

              if (toggle->on_toggle)
                toggle->on_toggle (toggle, toggle->state, toggle->on_toggle_data);

              g_print ("Toggle click\n");

              g_slice_free (ToggleGrabState, state);

              rig_shell_queue_redraw (toggle->ctx->shell);

              toggle->tentative_set = FALSE;
            }

          return RIG_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (rig_motion_event_get_action (event) == RIG_MOTION_EVENT_ACTION_MOVE)
        {
          float x = rig_motion_event_get_x (event);
          float y = rig_motion_event_get_y (event);

         if (rig_camera_pick_input_region (state->camera,
                                           state->region,
                                           x, y))
           toggle->tentative_set = TRUE;
         else
           toggle->tentative_set = FALSE;

          rig_shell_queue_redraw (toggle->ctx->shell);

          return RIG_INPUT_EVENT_STATUS_HANDLED;
        }
    }

  return RIG_INPUT_EVENT_STATUS_UNHANDLED;
}

static RigInputEventStatus
_rig_toggle_input_cb (RigInputRegion *region,
                      RigInputEvent *event,
                      void *user_data)
{
  RigToggle *toggle = user_data;

  g_print ("Toggle input\n");

  if(rig_input_event_get_type (event) == RIG_INPUT_EVENT_TYPE_MOTION &&
     rig_motion_event_get_action (event) == RIG_MOTION_EVENT_ACTION_DOWN)
    {
      RigShell *shell = toggle->ctx->shell;
      ToggleGrabState *state = g_slice_new (ToggleGrabState);

      state->toggle = toggle;
      state->camera = rig_input_event_get_camera (event);
      state->region = region;

      rig_shell_grab_input (shell, _rig_toggle_grab_input_cb, state);

      toggle->tentative_set = TRUE;

      rig_shell_queue_redraw (toggle->ctx->shell);

      return RIG_INPUT_EVENT_STATUS_HANDLED;
    }

  return RIG_INPUT_EVENT_STATUS_UNHANDLED;
}

static void
_rig_toggle_update_colours (RigToggle *toggle)
{
  uint32_t colors[2][2][3] =
    {
        /* Disabled */
        {
            /* Unset */
            {
                0x000000ff, /* border */
                0xffffffff, /* box */
                0x000000ff /* text*/
            },
            /* Set */
            {
                0x000000ff, /* border */
                0xffffffff, /* box */
                0x000000ff /* text*/
            },

        },
        /* Enabled */
        {
            /* Unset */
            {
                0x000000ff, /* border */
                0xffffffff, /* box */
                0x000000ff /* text*/
            },
            /* Set */
            {
                0x000000ff, /* border */
                0xffffffff, /* box */
                0x000000ff /* text*/
            },
        }
    };

  uint32_t border, box, text;

  border = colors[toggle->enabled][toggle->state][0];
  box = colors[toggle->enabled][toggle->state][1];
  text = colors[toggle->enabled][toggle->state][2];

  cogl_pipeline_set_color4f (toggle->pipeline_border,
                             RIG_UINT32_RED_AS_FLOAT (border),
                             RIG_UINT32_GREEN_AS_FLOAT (border),
                             RIG_UINT32_BLUE_AS_FLOAT (border),
                             RIG_UINT32_ALPHA_AS_FLOAT (border));
  cogl_pipeline_set_color4f (toggle->pipeline_box,
                             RIG_UINT32_RED_AS_FLOAT (box),
                             RIG_UINT32_GREEN_AS_FLOAT (box),
                             RIG_UINT32_BLUE_AS_FLOAT (box),
                             RIG_UINT32_ALPHA_AS_FLOAT (box));
  cogl_color_init_from_4f (&toggle->text_color,
                           RIG_UINT32_RED_AS_FLOAT (text),
                           RIG_UINT32_GREEN_AS_FLOAT (text),
                           RIG_UINT32_BLUE_AS_FLOAT (text),
                           RIG_UINT32_ALPHA_AS_FLOAT (text));
}

RigToggle *
rig_toggle_new (RigContext *ctx,
                const char *label)
{
  RigToggle *toggle = g_slice_new0 (RigToggle);
  PangoRectangle label_size;

  rig_object_init (RIG_OBJECT (toggle), &rig_toggle_type);

  toggle->ref_count = 1;

  rig_graphable_init (toggle);
  rig_paintable_init (toggle);

  rig_simple_introspectable_init (toggle,
                                  _rig_toggle_prop_specs,
                                  toggle->properties);

  toggle->ctx = ctx;

  toggle->state = TRUE;
  toggle->enabled = TRUE;

  toggle->on_toggle = NULL;
  toggle->on_toggle_data = NULL;

  toggle->tick = pango_layout_new (ctx->pango_context);
  pango_layout_set_font_description (toggle->tick, ctx->pango_font_desc);
  pango_layout_set_text (toggle->tick, "✔", -1);

  toggle->label = pango_layout_new (ctx->pango_context);
  pango_layout_set_font_description (toggle->label, ctx->pango_font_desc);
  pango_layout_set_text (toggle->label, label, -1);

  pango_layout_get_extents (toggle->label, NULL, &label_size);
  toggle->label_width = PANGO_PIXELS (label_size.width);
  toggle->label_height = PANGO_PIXELS (label_size.height);

  toggle->width = toggle->label_width + 10 + RIG_TOGGLE_BOX_WIDTH;
  toggle->height = toggle->label_height + 23;

  toggle->pipeline_border = cogl_pipeline_new (ctx->cogl_context);
  toggle->pipeline_box = cogl_pipeline_new (ctx->cogl_context);

  _rig_toggle_update_colours (toggle);

  toggle->input_region =
    rig_input_region_new_rectangle (0, 0,
                                    RIG_TOGGLE_BOX_WIDTH,
                                    RIG_TOGGLE_BOX_WIDTH,
                                    _rig_toggle_input_cb,
                                    toggle);

  //rig_input_region_set_graphable (toggle->input_region, toggle);
  rig_graphable_add_child (toggle, toggle->input_region);

  return toggle;
}

void
rig_toggle_set_on_toggle_callback (RigToggle *toggle,
                                   RigToggleCallback callback,
                                   void *user_data)
{
  g_return_if_fail (toggle->on_toggle == NULL || callback == NULL);

  toggle->on_toggle = callback;
  toggle->on_toggle_data = user_data;
}

void
rig_toggle_set_enabled (RigToggle *toggle,
                        CoglBool enabled)
{
  if (toggle->enabled == enabled)
    return;

  toggle->enabled = enabled;
  rig_property_dirty (&toggle->ctx->property_ctx,
                      &toggle->properties[RIG_TOGGLE_PROP_ENABLED]);
  rig_shell_queue_redraw (toggle->ctx->shell);
}

void
rig_toggle_set_state (RigToggle *toggle,
                      CoglBool state)
{
  if (toggle->state == state)
    return;

  toggle->state = state;
  rig_property_dirty (&toggle->ctx->property_ctx,
                      &toggle->properties[RIG_TOGGLE_PROP_STATE]);
  rig_shell_queue_redraw (toggle->ctx->shell);
}

RigProperty *
rig_toggle_get_enabled_property (RigToggle *toggle)
{
  return &toggle->properties[RIG_TOGGLE_PROP_STATE];
}

struct _RigUIViewport
{
  RigObjectProps _parent;

  RigContext *ctx;

  int ref_count;

  RigGraphableProps graphable;

  float width;
  float height;

  float doc_x;
  float doc_y;
  float doc_scale_x;
  float doc_scale_y;

  RigTransform *doc_transform;

  RigInputRegion *input_region;
  float grab_x;
  float grab_y;
  float grab_doc_x;
  float grab_doc_y;

};

static void
_rig_ui_viewport_free (void *object)
{
  RigUIViewport *ui_viewport = object;

  rig_ref_countable_simple_unref (ui_viewport->input_region);

  g_slice_free (RigUIViewport, object);
}

static RigRefCountableVTable _rig_ui_viewport_ref_countable_vtable = {
  rig_ref_countable_simple_ref,
  rig_ref_countable_simple_unref,
  _rig_ui_viewport_free
};

static RigGraphableVTable _rig_ui_viewport_graphable_vtable = {
  NULL, /* child_removed */
  NULL, /* child_added */
  NULL, /* parent_changed */
};

RigType rig_ui_viewport_type;

static void
_rig_ui_viewport_init_type (void)
{
  rig_type_init (&rig_ui_viewport_type);
  rig_type_add_interface (&rig_ui_viewport_type,
                          RIG_INTERFACE_ID_REF_COUNTABLE,
                          offsetof (RigUIViewport, ref_count),
                          &_rig_ui_viewport_ref_countable_vtable);
  rig_type_add_interface (&rig_ui_viewport_type,
                          RIG_INTERFACE_ID_GRAPHABLE,
                          offsetof (RigUIViewport, graphable),
                          &_rig_ui_viewport_graphable_vtable);
}

static void
_rig_ui_viewport_update_doc_matrix (RigUIViewport *ui_viewport)
{
  rig_transform_init_identity (ui_viewport->doc_transform);
  rig_transform_translate (ui_viewport->doc_transform,
                           ui_viewport->doc_x,
                           ui_viewport->doc_y,
                           0);
  rig_transform_scale (ui_viewport->doc_transform,
                       ui_viewport->doc_scale_x,
                       ui_viewport->doc_scale_y,
                       1);
}

static RigInputEventStatus
ui_viewport_grab_input_cb (RigInputEvent *event, void *user_data)
{
  RigUIViewport *ui_viewport = user_data;

  if (rig_input_event_get_type (event) != RIG_INPUT_EVENT_TYPE_MOTION)
    return RIG_INPUT_EVENT_STATUS_UNHANDLED;

  if (rig_motion_event_get_action (event) == RIG_MOTION_EVENT_ACTION_MOVE)
    {
      RigButtonState state = rig_motion_event_get_button_state (event);

      if (state & RIG_BUTTON_STATE_2)
        {
          float x = rig_motion_event_get_x (event);
          float y = rig_motion_event_get_y (event);
          float dx = x - ui_viewport->grab_x;
          float dy = y - ui_viewport->grab_y;
          float x_scale = rig_ui_viewport_get_doc_scale_x (ui_viewport);
          float y_scale = rig_ui_viewport_get_doc_scale_y (ui_viewport);
          float inv_x_scale = 1.0 / x_scale;
          float inv_y_scale = 1.0 / y_scale;

          rig_ui_viewport_set_doc_x (ui_viewport,
                                     ui_viewport->grab_doc_x + (dx * inv_x_scale));
          rig_ui_viewport_set_doc_y (ui_viewport,
                                     ui_viewport->grab_doc_y + (dy * inv_y_scale));

          rig_shell_queue_redraw (ui_viewport->ctx->shell);
          return RIG_INPUT_EVENT_STATUS_HANDLED;
        }
    }
  else if (rig_motion_event_get_action (event) == RIG_MOTION_EVENT_ACTION_UP)
    {
      rig_shell_ungrab_input (ui_viewport->ctx->shell);
      return RIG_INPUT_EVENT_STATUS_HANDLED;
    }

  return RIG_INPUT_EVENT_STATUS_UNHANDLED;
}
static RigInputEventStatus
_rig_ui_viewport_input_cb (RigInputRegion *region,
                           RigInputEvent *event,
                           void *user_data)
{
  RigUIViewport *ui_viewport = user_data;

  g_print ("viewport input\n");
  if (rig_input_event_get_type (event) == RIG_INPUT_EVENT_TYPE_MOTION)
    {
      switch (rig_motion_event_get_action (event))
        {
        case RIG_MOTION_EVENT_ACTION_DOWN:
          {
            RigButtonState state = rig_motion_event_get_button_state (event);

            if (state & RIG_BUTTON_STATE_2)
              {
                ui_viewport->grab_x = rig_motion_event_get_x (event);
                ui_viewport->grab_y = rig_motion_event_get_y (event);

                ui_viewport->grab_doc_x = rig_ui_viewport_get_doc_x (ui_viewport);
                ui_viewport->grab_doc_y = rig_ui_viewport_get_doc_y (ui_viewport);

                /* TODO: Add rig_shell_implicit_grab_input() that handles releasing
                 * the grab for you */
                g_print ("viewport input grab\n");
                rig_shell_grab_input (ui_viewport->ctx->shell,
                                      ui_viewport_grab_input_cb, ui_viewport);
                return RIG_INPUT_EVENT_STATUS_HANDLED;
              }
          }
	default:
	  break;
        }
    }

  return RIG_INPUT_EVENT_STATUS_UNHANDLED;
}

RigUIViewport *
rig_ui_viewport_new (RigContext *ctx,
                     float width,
                     float height,
                     ...)
{
  RigUIViewport *ui_viewport = g_slice_new0 (RigUIViewport);
  va_list ap;
  RigObject *object;

  rig_object_init (RIG_OBJECT (ui_viewport), &rig_ui_viewport_type);

  ui_viewport->ctx = ctx;

  ui_viewport->ref_count = 1;

  rig_graphable_init (RIG_OBJECT (ui_viewport));

  ui_viewport->width = width;
  ui_viewport->height = height;
  ui_viewport->doc_x = 0;
  ui_viewport->doc_y = 0;
  ui_viewport->doc_scale_x = 1;
  ui_viewport->doc_scale_y = 1;

  ui_viewport->doc_transform = rig_transform_new (ctx, NULL);
  rig_graphable_add_child (ui_viewport, ui_viewport->doc_transform);

  _rig_ui_viewport_update_doc_matrix (ui_viewport);

  ui_viewport->input_region =
    rig_input_region_new_rectangle (0, 0,
                                    ui_viewport->width,
                                    ui_viewport->height,
                                    _rig_ui_viewport_input_cb,
                                    ui_viewport);

  //rig_input_region_set_graphable (ui_viewport->input_region, ui_viewport);
  rig_graphable_add_child (ui_viewport, ui_viewport->input_region);

  va_start (ap, height);
  while ((object = va_arg (ap, RigObject *)))
    rig_graphable_add_child (RIG_OBJECT (ui_viewport), object);
  va_end (ap);

  return ui_viewport;
}

void
rig_ui_viewport_set_width (RigUIViewport *ui_viewport, float width)
{
  ui_viewport->width = width;

  rig_input_region_set_rectangle (ui_viewport->input_region,
                                  0, 0,
                                  ui_viewport->width,
                                  ui_viewport->height);
}

void
rig_ui_viewport_set_height (RigUIViewport *ui_viewport, float height)
{
  ui_viewport->height = height;

  rig_input_region_set_rectangle (ui_viewport->input_region,
                                  0, 0,
                                  ui_viewport->width,
                                  ui_viewport->height);
}

void
rig_ui_viewport_set_doc_x (RigUIViewport *ui_viewport, float doc_x)
{
  ui_viewport->doc_x = doc_x;
  _rig_ui_viewport_update_doc_matrix (ui_viewport);
}

void
rig_ui_viewport_set_doc_y (RigUIViewport *ui_viewport, float doc_y)
{
  g_print ("ui_viewport doc_y = %f\n", doc_y);
  ui_viewport->doc_y = doc_y;
  _rig_ui_viewport_update_doc_matrix (ui_viewport);
}

void
rig_ui_viewport_set_doc_scale_x (RigUIViewport *ui_viewport, float doc_scale_x)
{
  ui_viewport->doc_scale_x = doc_scale_x;
  _rig_ui_viewport_update_doc_matrix (ui_viewport);
}

void
rig_ui_viewport_set_doc_scale_y (RigUIViewport *ui_viewport, float doc_scale_y)
{
  ui_viewport->doc_scale_y = doc_scale_y;
  _rig_ui_viewport_update_doc_matrix (ui_viewport);
}

float
rig_ui_viewport_get_width (RigUIViewport *ui_viewport)
{
  return ui_viewport->width;
}

float
rig_ui_viewport_get_height (RigUIViewport *ui_viewport)
{
  return ui_viewport->height;
}

float
rig_ui_viewport_get_doc_x (RigUIViewport *ui_viewport)
{
  return ui_viewport->doc_x;
}

float
rig_ui_viewport_get_doc_y (RigUIViewport *ui_viewport)
{
  return ui_viewport->doc_y;
}

float
rig_ui_viewport_get_doc_scale_x (RigUIViewport *ui_viewport)
{
  return ui_viewport->doc_scale_x;
}

float
rig_ui_viewport_get_doc_scale_y (RigUIViewport *ui_viewport)
{
  return ui_viewport->doc_scale_y;
}

const CoglMatrix *
rig_ui_viewport_get_doc_matrix (RigUIViewport *ui_viewport)
{
  return rig_transform_get_matrix (ui_viewport->doc_transform);
}

RigObject *
rig_ui_viewport_get_doc_node (RigUIViewport *ui_viewport)
{
  return ui_viewport->doc_transform;
}

#if 0
static void
_rig_text_free (void *object)
{
  RigText *text = object;

  g_object_unref (text->text);

  g_slice_free (RigText, object);
}

RigRefCountableVTable _rig_text_ref_countable_vtable = {
  rig_ref_countable_simple_ref,
  rig_ref_countable_simple_unref,
  _rig_text_free
};

static RigGraphableVTable _rig_text_graphable_vtable = {
  NULL, /* child remove */
  NULL, /* child add */
  NULL /* parent changed */
};

static void
_rig_text_paint (RigObject *object,
                 RigPaintContext *paint_ctx)
{
  RigText *text = RIG_TEXT (object);
  RigCamera *camera = paint_ctx->camera;

  cogl_pango_show_layout (camera->fb,
                          text->text,
                          0, 0,
                          &text->text_color);
}

static RigPaintableVTable _rig_text_paintable_vtable = {
  _rig_text_paint
};

RigSimpleWidgetVTable _rig_text_simple_widget_vtable = {
 0
};

RigType rig_text_type;

static void
_rig_text_init_type (void)
{
  rig_type_init (&rig_text_type);
  rig_type_add_interface (&rig_text_type,
                          RIG_INTERFACE_ID_REF_COUNTABLE,
                          offsetof (RigText, ref_count),
                          &_rig_text_ref_countable_vtable);
  rig_type_add_interface (&rig_text_type,
                          RIG_INTERFACE_ID_GRAPHABLE,
                          offsetof (RigText, graphable),
                          &_rig_text_graphable_vtable);
  rig_type_add_interface (&rig_text_type,
                          RIG_INTERFACE_ID_PAINTABLE,
                          offsetof (RigText, paintable),
                          &_rig_text_paintable_vtable);
}

static RigInputEventStatus
_rig_text_input_cb (RigInputRegion *region,
                      RigInputEvent *event,
                      void *user_data)
{
  //RigText *text = user_data;

  g_print ("Text input\n");

  return RIG_INPUT_EVENT_STATUS_UNHANDLED;
}

RigText *
rig_text_new (RigContext *ctx,
              const char *text_str)
{
  RigText *text = g_slice_new0 (RigText);
  PangoRectangle text_size;

  rig_object_init (RIG_OBJECT (text), &rig_text_type);

  text->ref_count = 1;

  rig_graphable_init (RIG_OBJECT (text));
  rig_paintable_init (RIG_OBJECT (text));

  text->ctx = ctx;

  text->text = pango_layout_new (ctx->pango_context);
  pango_layout_set_font_description (text->text, ctx->pango_font_desc);
  pango_layout_set_text (text->text, text_str, -1);

  pango_layout_get_extents (text->text, NULL, &text_size);
  text->text_width = PANGO_PIXELS (text_size.width);
  text->text_height = PANGO_PIXELS (text_size.height);

  text->width = text->text_width + 10;
  text->height = text->text_height + 23;

  cogl_color_init_from_4f (&text->text_color, 0, 0, 0, 1);

  text->input_region =
    rig_input_region_new_rectangle (0, 0, text->width, text->height,
                                    _rig_text_input_cb,
                                    text);

  //rig_input_region_set_graphable (text->input_region, text);
  rig_graphable_add_child (text, text->input_region);

  return text;
}
#endif

static void
_rig_button_free (void *object)
{
  RigButton *button = object;

  rig_ref_countable_unref (button->background_normal);
  rig_ref_countable_unref (button->background_hover);
  rig_ref_countable_unref (button->background_active);
  rig_ref_countable_unref (button->background_disabled);

  g_object_unref (button->label);

  g_slice_free (RigButton, object);
}

RigRefCountableVTable _rig_button_ref_countable_vtable = {
  rig_ref_countable_simple_ref,
  rig_ref_countable_simple_unref,
  _rig_button_free
};

static RigGraphableVTable _rig_button_graphable_vtable = {
  NULL, /* child remove */
  NULL, /* child add */
  NULL /* parent changed */
};

static void
_rig_button_paint (RigObject *object,
                   RigPaintContext *paint_ctx)
{
  RigButton *button = RIG_BUTTON (object);
  RigCamera *camera = paint_ctx->camera;
  RigPaintableVTable *bg_paintable =
    rig_object_get_vtable (button->background_normal, RIG_INTERFACE_ID_PAINTABLE);

  switch (button->state)
    {
    case BUTTON_STATE_NORMAL:
      bg_paintable->paint (RIG_OBJECT (button->background_normal), paint_ctx);
      break;
    case BUTTON_STATE_HOVER:
      bg_paintable->paint (RIG_OBJECT (button->background_hover), paint_ctx);
      break;
    case BUTTON_STATE_ACTIVE:
      bg_paintable->paint (RIG_OBJECT (button->background_active), paint_ctx);
      break;
    case BUTTON_STATE_ACTIVE_CANCEL:
      bg_paintable->paint (RIG_OBJECT (button->background_active), paint_ctx);
      break;
    case BUTTON_STATE_DISABLED:
      bg_paintable->paint (RIG_OBJECT (button->background_disabled), paint_ctx);
      break;
    }

  cogl_pango_show_layout (camera->fb,
                          button->label,
                          5, 11,
                          &button->text_color);
}

static RigPaintableVTable _rig_button_paintable_vtable = {
  _rig_button_paint
};

RigSimpleWidgetVTable _rig_button_simple_widget_vtable = {
 0
};

RigType rig_button_type;

static void
_rig_button_init_type (void)
{
  rig_type_init (&rig_button_type);
  rig_type_add_interface (&rig_button_type,
                          RIG_INTERFACE_ID_REF_COUNTABLE,
                          offsetof (RigButton, ref_count),
                          &_rig_button_ref_countable_vtable);
  rig_type_add_interface (&rig_button_type,
                          RIG_INTERFACE_ID_GRAPHABLE,
                          offsetof (RigButton, graphable),
                          &_rig_button_graphable_vtable);
  rig_type_add_interface (&rig_button_type,
                          RIG_INTERFACE_ID_PAINTABLE,
                          offsetof (RigButton, paintable),
                          &_rig_button_paintable_vtable);
  rig_type_add_interface (&rig_button_type,
                          RIG_INTERFACE_ID_SIMPLE_WIDGET,
                          offsetof (RigButton, simple_widget),
                          &_rig_button_simple_widget_vtable);
}

typedef struct _ButtonGrabState
{
  RigCamera *camera;
  RigButton *button;
  CoglMatrix transform;
  CoglMatrix inverse_transform;
} ButtonGrabState;

static RigInputEventStatus
_rig_button_grab_input_cb (RigInputEvent *event,
                           void *user_data)
{
  ButtonGrabState *state = user_data;
  RigButton *button = state->button;

  if(rig_input_event_get_type (event) == RIG_INPUT_EVENT_TYPE_MOTION)
    {
      RigShell *shell = button->ctx->shell;
      if (rig_motion_event_get_action (event) == RIG_MOTION_EVENT_ACTION_UP)
        {
          rig_shell_ungrab_input (shell);

          if (button->on_click)
            button->on_click (button, button->on_click_data);

          g_print ("Button click\n");

          g_slice_free (ButtonGrabState, state);

          button->state = BUTTON_STATE_NORMAL;
          rig_shell_queue_redraw (button->ctx->shell);

          return RIG_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (rig_motion_event_get_action (event) == RIG_MOTION_EVENT_ACTION_MOVE)
        {
          float x = rig_motion_event_get_x (event);
          float y = rig_motion_event_get_y (event);
          RigCamera *camera = state->camera;

          rig_camera_unproject_coord (camera,
                                      &state->transform,
                                      &state->inverse_transform,
                                      0,
                                      &x,
                                      &y);

          if (x < 0 || x > button->width ||
              y < 0 || y > button->height)
            button->state = BUTTON_STATE_ACTIVE_CANCEL;
          else
            button->state = BUTTON_STATE_ACTIVE;

          rig_shell_queue_redraw (button->ctx->shell);

          return RIG_INPUT_EVENT_STATUS_HANDLED;
        }
    }

  return RIG_INPUT_EVENT_STATUS_UNHANDLED;
}

static RigInputEventStatus
_rig_button_input_cb (RigInputRegion *region,
                      RigInputEvent *event,
                      void *user_data)
{
  RigButton *button = user_data;

  g_print ("Button input\n");

  if(rig_input_event_get_type (event) == RIG_INPUT_EVENT_TYPE_MOTION &&
     rig_motion_event_get_action (event) == RIG_MOTION_EVENT_ACTION_DOWN)
    {
      RigShell *shell = button->ctx->shell;
      ButtonGrabState *state = g_slice_new (ButtonGrabState);
      const CoglMatrix *view;

      state->button = button;
      state->camera = rig_input_event_get_camera (event);
      view = rig_camera_get_view_transform (state->camera);
      state->transform = *view;
      rig_graphable_apply_transform (button, &state->transform);
      if (!cogl_matrix_get_inverse (&state->transform,
                                    &state->inverse_transform))
        {
          g_warning ("Failed to calculate inverse of button transform\n");
          g_slice_free (ButtonGrabState, state);
          return RIG_INPUT_EVENT_STATUS_UNHANDLED;
        }

      rig_shell_grab_input (shell, _rig_button_grab_input_cb, state);
      //button->grab_x = rig_motion_event_get_x (event);
      //button->grab_y = rig_motion_event_get_y (event);

      button->state = BUTTON_STATE_ACTIVE;
      rig_shell_queue_redraw (button->ctx->shell);

      return RIG_INPUT_EVENT_STATUS_HANDLED;
    }

  return RIG_INPUT_EVENT_STATUS_UNHANDLED;
}

RigButton *
rig_button_new (RigContext *ctx,
                const char *label)
{
  RigButton *button = g_slice_new0 (RigButton);
  CoglTexture *normal_texture;
  CoglTexture *hover_texture;
  CoglTexture *active_texture;
  CoglTexture *disabled_texture;
  GError *error = NULL;
  PangoRectangle label_size;

  rig_object_init (RIG_OBJECT (button), &rig_button_type);

  button->ref_count = 1;

  rig_graphable_init (RIG_OBJECT (button));
  rig_paintable_init (RIG_OBJECT (button));

  button->ctx = ctx;

  button->state = BUTTON_STATE_NORMAL;

  button->on_click = NULL;
  button->on_click_data = NULL;

  normal_texture = rig_load_texture (ctx, RIG_DATA_DIR "button.png", &error);
  if (!normal_texture)
    {
      g_warning ("Failed to load button texture: %s", error->message);
      g_error_free (error);
    }

  hover_texture = rig_load_texture (ctx, RIG_DATA_DIR "button-hover.png", &error);
  if (!hover_texture)
    {
      cogl_object_unref (normal_texture);
      g_warning ("Failed to load button-hover texture: %s", error->message);
      g_error_free (error);
    }

  active_texture = rig_load_texture (ctx, RIG_DATA_DIR "button-active.png", &error);
  if (!active_texture)
    {
      cogl_object_unref (normal_texture);
      cogl_object_unref (hover_texture);
      g_warning ("Failed to load button-active texture: %s", error->message);
      g_error_free (error);
    }

  disabled_texture = rig_load_texture (ctx, RIG_DATA_DIR "button-disabled.png", &error);
  if (!disabled_texture)
    {
      cogl_object_unref (normal_texture);
      cogl_object_unref (hover_texture);
      cogl_object_unref (active_texture);
      g_warning ("Failed to load button-disabled texture: %s", error->message);
      g_error_free (error);
    }

  button->label = pango_layout_new (ctx->pango_context);
  pango_layout_set_font_description (button->label, ctx->pango_font_desc);
  pango_layout_set_text (button->label, label, -1);

  pango_layout_get_extents (button->label, NULL, &label_size);
  button->label_width = PANGO_PIXELS (label_size.width);
  button->label_height = PANGO_PIXELS (label_size.height);

  button->width = button->label_width + 10;
  button->height = button->label_height + 23;

  button->background_normal =
    rig_nine_slice_new (ctx, normal_texture, 11, 5, 13, 5,
                        button->width,
                        button->height);

  button->background_hover =
    _rig_nine_slice_new_full (ctx, hover_texture, 11, 5, 13, 5,
                              button->width,
                              button->height,
                              button->background_normal->primitive);

  button->background_active =
    _rig_nine_slice_new_full (ctx, active_texture, 11, 5, 13, 5,
                              button->width,
                              button->height,
                              button->background_normal->primitive);

  button->background_disabled =
    _rig_nine_slice_new_full (ctx, disabled_texture, 11, 5, 13, 5,
                              button->width,
                              button->height,
                              button->background_normal->primitive);

  cogl_color_init_from_4f (&button->text_color, 0, 0, 0, 1);

  button->input_region =
    rig_input_region_new_rectangle (0, 0, button->width, button->height,
                                    _rig_button_input_cb,
                                    button);

  //rig_input_region_set_graphable (button->input_region, button);
  rig_graphable_add_child (button, button->input_region);

  return button;
}

void
rig_button_set_on_click_callback (RigButton *button,
                                  RigButtonClickCallback callback,
                                  void *user_data)
{
  g_return_if_fail (button->on_click == NULL || callback == NULL);

  button->on_click = callback;
  button->on_click_data = user_data;
}


/*
 * TODO:
 *
 * Should we add a _queue_batch_update() mechanism or should scene
 * changing events just immediately modify the display lists?
 * - An advantage of deferring is that it can avoid potentially
 *   redundant work.
 * - A difficulty with this currently is that there isn't a way
 *   access the camera associated with a node in the graph.
 *
 * Should we add a "Widgetable" interface that implies:
 * - RefCountable
 * - Graphable
 * - PaintBatchable
 *
 * There are probably lots of utility apis we could add too for
 * widgets.
 *
 * Can we figure out a neat way of handling Cloning?
 * Can we figure out a neat way of handling per-camera state for
 * widgets?
 */
/*

When we paint we should paint an ordered list of cameras

Questions:
- Where should logic for picking and tracking if we have a valid pick buffer live?
-

Think about this idea of the "div" graph that feeds into a separate spacial graph which feeds into a render graph

-----

If I wanted a visual tool where I could have prototyped the hairy cube code how might that work:

- Some ui to setup a viewing frustum
- Some ui to add geometry to the scene - a few toy models such as spheres, pyramids cubes would have been fine here
- Some ui to render the noise textures
  - ui to create a texture
  - ui to create a camera around texture
  - DESC
  - ui to describe what to render to the camera
  - ui to add a random number generator
  -
- Some ui to describe N different pipelines - one for each shell
- Some ui to describe what to draw for a frame
  - An ordered list of drawing commands:
    - draw geometry X with pipeline Y
    - draw geometry A with pipeline B
    - ...

*/

void
_rig_init (void)
{
  static size_t init_status = 0;

  if (g_once_init_enter (&init_status))
    {
      //bindtextdomain (GETTEXT_PACKAGE, RIG_LOCALEDIR);
      //bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

      g_type_init ();

      _rig_context_init_type ();
      _rig_camera_init_type ();
      _rig_nine_slice_init_type ();
      _rig_rectangle_init_type ();
      _rig_text_buffer_init_type ();
      _rig_text_init_type ();
      _rig_button_init_type ();
      _rig_toggle_init_type ();
      _rig_graph_init_type ();
      _rig_transform_init_type ();
      _rig_timeline_init_type ();
      _rig_ui_viewport_init_type ();
      _rig_entity_init_type ();

      /* components */
      _rig_animation_clip_init_type ();
      _rig_light_init_type ();
      _rig_mesh_renderer_init_type ();
      _rig_material_init_type ();

      g_once_init_leave (&init_status, 1);
    }
}

void
rig_color_init_from_uint32 (RigColor *color, uint32_t value)
{
  color->red = RIG_UINT32_RED_AS_FLOAT (value);
  color->green = RIG_UINT32_GREEN_AS_FLOAT (value);
  color->blue = RIG_UINT32_BLUE_AS_FLOAT (value);
  color->alpha = RIG_UINT32_ALPHA_AS_FLOAT (value);
}
