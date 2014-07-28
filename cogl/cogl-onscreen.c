/*
 * Cogl
 *
 * A Low-Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2011, 2013 Intel Corporation.
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
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl-util.h"
#include "cogl-onscreen-private.h"
#include "cogl-frame-info-private.h"
#include "cogl-framebuffer-private.h"
#include "cogl-onscreen-template-private.h"
#include "cogl-device-private.h"
#include "cogl-object-private.h"
#include "cogl-closure-list-private.h"
#include "cogl-poll-private.h"

static void _cg_onscreen_free(cg_onscreen_t *onscreen);

CG_OBJECT_DEFINE_WITH_CODE(Onscreen,
                           onscreen,
                           _cg_onscreen_class.virt_unref =
                               _cg_framebuffer_unref);

static void
_cg_onscreen_init_from_template(cg_onscreen_t *onscreen,
                                cg_onscreen_template_t *onscreen_template)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);

    _cg_list_init(&onscreen->frame_closures);
    _cg_list_init(&onscreen->resize_closures);
    _cg_list_init(&onscreen->dirty_closures);

    framebuffer->config = onscreen_template->config;
}

cg_onscreen_t *
cg_onscreen_new(cg_device_t *dev, int width, int height)
{
    cg_onscreen_t *onscreen;

    /* FIXME: We are assuming onscreen buffers will always be
       premultiplied so we'll set the premult flag on the bitmap
       format. This will usually be correct because the result of the
       default blending operations for Cogl ends up with premultiplied
       data in the framebuffer. However it is possible for the
       framebuffer to be in whatever format depending on what
       cg_pipeline_t is used to render to it. Eventually we may want to
       add a way for an application to inform Cogl that the framebuffer
       is not premultiplied in case it is being used for some special
       purpose. */

    onscreen = c_new0(cg_onscreen_t, 1);
    _cg_framebuffer_init(CG_FRAMEBUFFER(onscreen),
                         dev,
                         CG_FRAMEBUFFER_TYPE_ONSCREEN,
                         width, /* width */
                         height); /* height */

    _cg_onscreen_init_from_template(onscreen, dev->display->onscreen_template);

    return _cg_onscreen_object_new(onscreen);
}

static void
_cg_onscreen_free(cg_onscreen_t *onscreen)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);
    const cg_winsys_vtable_t *winsys = _cg_framebuffer_get_winsys(framebuffer);
    cg_frame_info_t *frame_info;

    _cg_closure_list_disconnect_all(&onscreen->resize_closures);
    _cg_closure_list_disconnect_all(&onscreen->frame_closures);
    _cg_closure_list_disconnect_all(&onscreen->dirty_closures);

    while ((frame_info = c_queue_pop_tail(&onscreen->pending_frame_infos)))
        cg_object_unref(frame_info);
    c_queue_clear(&onscreen->pending_frame_infos);

    winsys->onscreen_deinit(onscreen);
    c_return_if_fail(onscreen->winsys == NULL);

    /* Chain up to parent */
    _cg_framebuffer_free(framebuffer);

    c_free(onscreen);
}

static void
notify_event(cg_onscreen_t *onscreen,
             cg_frame_event_t event,
             cg_frame_info_t *info)
{
    _cg_closure_list_invoke(
        &onscreen->frame_closures, cg_frame_callback_t, onscreen, event, info);
}

static void
_cg_dispatch_onscreen_cb(cg_device_t *dev)
{
    cg_onscreen_event_t *event, *tmp;
    cg_list_t queue;

    /* Dispatching the event callback may cause another frame to be
     * drawn which in may cause another event to be queued immediately.
     * To make sure this loop will only dispatch one set of events we'll
     * steal the queue and iterate that separately */
    _cg_list_init(&queue);
    _cg_list_insert_list(&queue, &dev->onscreen_events_queue);
    _cg_list_init(&dev->onscreen_events_queue);

    _cg_closure_disconnect(dev->onscreen_dispatch_idle);
    dev->onscreen_dispatch_idle = NULL;

    _cg_list_for_each_safe(event, tmp, &queue, link)
    {
        cg_onscreen_t *onscreen = event->onscreen;
        cg_frame_info_t *info = event->info;

        notify_event(onscreen, event->type, info);

        cg_object_unref(onscreen);
        cg_object_unref(info);

        c_slice_free(cg_onscreen_event_t, event);
    }

    while (!_cg_list_empty(&dev->onscreen_dirty_queue)) {
        cg_onscreen_queued_dirty_t *qe =
            _cg_container_of(dev->onscreen_dirty_queue.next,
                             cg_onscreen_queued_dirty_t,
                             link);

        _cg_list_remove(&qe->link);

        _cg_closure_list_invoke(&qe->onscreen->dirty_closures,
                                cg_onscreen_dirty_callback_t,
                                qe->onscreen,
                                &qe->info);

        cg_object_unref(qe->onscreen);

        c_slice_free(cg_onscreen_queued_dirty_t, qe);
    }
}

static void
_cg_onscreen_queue_dispatch_idle(cg_onscreen_t *onscreen)
{
    cg_device_t *dev = CG_FRAMEBUFFER(onscreen)->dev;

    if (!dev->onscreen_dispatch_idle) {
        dev->onscreen_dispatch_idle = _cg_poll_renderer_add_idle(dev->display->renderer,
            (cg_idle_callback_t)_cg_dispatch_onscreen_cb,
            dev,
            NULL);
    }
}

void
_cg_onscreen_queue_dirty(cg_onscreen_t *onscreen,
                         const cg_onscreen_dirty_info_t *info)
{
    cg_device_t *dev = CG_FRAMEBUFFER(onscreen)->dev;
    cg_onscreen_queued_dirty_t *qe = c_slice_new(cg_onscreen_queued_dirty_t);

    qe->onscreen = cg_object_ref(onscreen);
    qe->info = *info;
    _cg_list_insert(dev->onscreen_dirty_queue.prev, &qe->link);

    _cg_onscreen_queue_dispatch_idle(onscreen);
}

void
_cg_onscreen_queue_full_dirty(cg_onscreen_t *onscreen)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);
    cg_onscreen_dirty_info_t info;

    info.x = 0;
    info.y = 0;
    info.width = framebuffer->width;
    info.height = framebuffer->height;

    _cg_onscreen_queue_dirty(onscreen, &info);
}

void
_cg_onscreen_queue_event(cg_onscreen_t *onscreen,
                         cg_frame_event_t type,
                         cg_frame_info_t *info)
{
    cg_device_t *dev = CG_FRAMEBUFFER(onscreen)->dev;

    cg_onscreen_event_t *event = c_slice_new(cg_onscreen_event_t);

    event->onscreen = cg_object_ref(onscreen);
    event->info = cg_object_ref(info);
    event->type = type;

    _cg_list_insert(dev->onscreen_events_queue.prev, &event->link);

    _cg_onscreen_queue_dispatch_idle(onscreen);
}

void
cg_onscreen_swap_buffers_with_damage(cg_onscreen_t *onscreen,
                                     const int *rectangles,
                                     int n_rectangles)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);
    const cg_winsys_vtable_t *winsys;
    cg_frame_info_t *info;

    c_return_if_fail(framebuffer->type == CG_FRAMEBUFFER_TYPE_ONSCREEN);

    info = _cg_frame_info_new();
    info->frame_counter = onscreen->frame_counter;
    c_queue_push_tail(&onscreen->pending_frame_infos, info);

    _cg_framebuffer_flush_journal(framebuffer);

    winsys = _cg_framebuffer_get_winsys(framebuffer);
    winsys->onscreen_swap_buffers_with_damage(
        onscreen, rectangles, n_rectangles);
    cg_framebuffer_discard_buffers(framebuffer,
                                   CG_BUFFER_BIT_COLOR | CG_BUFFER_BIT_DEPTH |
                                   CG_BUFFER_BIT_STENCIL);

    if (!_cg_winsys_has_feature(CG_WINSYS_FEATURE_SYNC_AND_COMPLETE_EVENT)) {
        cg_frame_info_t *info;

        c_warn_if_fail(onscreen->pending_frame_infos.length == 1);

        info = c_queue_pop_tail(&onscreen->pending_frame_infos);

        _cg_onscreen_queue_event(onscreen, CG_FRAME_EVENT_SYNC, info);
        _cg_onscreen_queue_event(onscreen, CG_FRAME_EVENT_COMPLETE, info);

        cg_object_unref(info);
    }

    onscreen->frame_counter++;
    framebuffer->mid_scene = false;
}

void
cg_onscreen_swap_buffers(cg_onscreen_t *onscreen)
{
    cg_onscreen_swap_buffers_with_damage(onscreen, NULL, 0);
}

void
cg_onscreen_swap_region(cg_onscreen_t *onscreen,
                        const int *rectangles,
                        int n_rectangles)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);
    const cg_winsys_vtable_t *winsys;
    cg_frame_info_t *info;

    c_return_if_fail(framebuffer->type == CG_FRAMEBUFFER_TYPE_ONSCREEN);

    info = _cg_frame_info_new();
    info->frame_counter = onscreen->frame_counter;
    c_queue_push_tail(&onscreen->pending_frame_infos, info);

    _cg_framebuffer_flush_journal(framebuffer);

    winsys = _cg_framebuffer_get_winsys(framebuffer);

    /* This should only be called if the winsys advertises
       CG_WINSYS_FEATURE_SWAP_REGION */
    c_return_if_fail(winsys->onscreen_swap_region != NULL);

    winsys->onscreen_swap_region(
        CG_ONSCREEN(framebuffer), rectangles, n_rectangles);

    cg_framebuffer_discard_buffers(framebuffer,
                                   CG_BUFFER_BIT_COLOR | CG_BUFFER_BIT_DEPTH |
                                   CG_BUFFER_BIT_STENCIL);

    if (!_cg_winsys_has_feature(CG_WINSYS_FEATURE_SYNC_AND_COMPLETE_EVENT)) {
        cg_frame_info_t *info;

        c_warn_if_fail(onscreen->pending_frame_infos.length == 1);

        info = c_queue_pop_tail(&onscreen->pending_frame_infos);

        _cg_onscreen_queue_event(onscreen, CG_FRAME_EVENT_SYNC, info);
        _cg_onscreen_queue_event(onscreen, CG_FRAME_EVENT_COMPLETE, info);

        cg_object_unref(info);
    }

    onscreen->frame_counter++;
    framebuffer->mid_scene = false;
}

int
cg_onscreen_get_buffer_age(cg_onscreen_t *onscreen)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);
    const cg_winsys_vtable_t *winsys;

    c_return_val_if_fail(framebuffer->type == CG_FRAMEBUFFER_TYPE_ONSCREEN,
                           0);

    winsys = _cg_framebuffer_get_winsys(framebuffer);

    if (!winsys->onscreen_get_buffer_age)
        return 0;

    return winsys->onscreen_get_buffer_age(onscreen);
}

#ifdef CG_HAS_X11_SUPPORT
void
cg_x11_onscreen_set_foreign_window_xid(cg_onscreen_t *onscreen,
                                       uint32_t xid,
                                       cg_onscreen_x11_mask_callback_t update,
                                       void *user_data)
{
    /* We don't wan't applications to get away with being lazy here and not
     * passing an update callback... */
    c_return_if_fail(update);

    onscreen->foreign_xid = xid;
    onscreen->foreign_update_mask_callback = update;
    onscreen->foreign_update_mask_data = user_data;
}

uint32_t
cg_x11_onscreen_get_window_xid(cg_onscreen_t *onscreen)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);

    if (onscreen->foreign_xid)
        return onscreen->foreign_xid;
    else {
        const cg_winsys_vtable_t *winsys =
            _cg_framebuffer_get_winsys(framebuffer);

        /* This should only be called for x11 onscreens */
        c_return_val_if_fail(winsys->onscreen_x11_get_window_xid != NULL, 0);

        return winsys->onscreen_x11_get_window_xid(onscreen);
    }
}

uint32_t
cg_x11_onscreen_get_visual_xid(cg_onscreen_t *onscreen)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);
    const cg_winsys_vtable_t *winsys = _cg_framebuffer_get_winsys(framebuffer);
    XVisualInfo *visinfo;
    uint32_t id;

    /* This should only be called for xlib based onscreens */
    c_return_val_if_fail(winsys->xlib_get_visual_info != NULL, 0);

    visinfo = winsys->xlib_get_visual_info();
    id = (uint32_t)visinfo->visualid;

    XFree(visinfo);
    return id;
}
#endif /* CG_HAS_X11_SUPPORT */

#ifdef CG_HAS_WIN32_SUPPORT

void
cg_win32_onscreen_set_foreign_window(cg_onscreen_t *onscreen, HWND hwnd)
{
    onscreen->foreign_hwnd = hwnd;
}

HWND
cg_win32_onscreen_get_window(cg_onscreen_t *onscreen)
{
    if (onscreen->foreign_hwnd)
        return onscreen->foreign_hwnd;
    else {
        cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);
        const cg_winsys_vtable_t *winsys =
            _cg_framebuffer_get_winsys(framebuffer);

        /* This should only be called for win32 onscreens */
        c_return_val_if_fail(winsys->onscreen_win32_get_window != NULL, 0);

        return winsys->onscreen_win32_get_window(onscreen);
    }
}

#endif /* CG_HAS_WIN32_SUPPORT */

CoglFrameClosure *
cg_onscreen_add_frame_callback(cg_onscreen_t *onscreen,
                               cg_frame_callback_t callback,
                               void *user_data,
                               cg_user_data_destroy_callback_t destroy)
{
    return _cg_closure_list_add(
        &onscreen->frame_closures, callback, user_data, destroy);
}

void
cg_onscreen_remove_frame_callback(cg_onscreen_t *onscreen,
                                  CoglFrameClosure *closure)
{
    c_return_if_fail(closure);

    _cg_closure_disconnect(closure);
}

void
cg_onscreen_set_swap_throttled(cg_onscreen_t *onscreen, bool throttled)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);
    framebuffer->config.swap_throttled = throttled;
    if (framebuffer->allocated) {
        const cg_winsys_vtable_t *winsys =
            _cg_framebuffer_get_winsys(framebuffer);
        winsys->onscreen_update_swap_throttled(onscreen);
    }
}

void
cg_onscreen_show(cg_onscreen_t *onscreen)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);
    const cg_winsys_vtable_t *winsys;

    if (!framebuffer->allocated) {
        if (!cg_framebuffer_allocate(framebuffer, NULL))
            return;
    }

    winsys = _cg_framebuffer_get_winsys(framebuffer);
    if (winsys->onscreen_set_visibility)
        winsys->onscreen_set_visibility(onscreen, true);
}

void
cg_onscreen_hide(cg_onscreen_t *onscreen)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);

    if (framebuffer->allocated) {
        const cg_winsys_vtable_t *winsys =
            _cg_framebuffer_get_winsys(framebuffer);
        if (winsys->onscreen_set_visibility)
            winsys->onscreen_set_visibility(onscreen, false);
    }
}

void
_cg_onscreen_notify_frame_sync(cg_onscreen_t *onscreen,
                               cg_frame_info_t *info)
{
    notify_event(onscreen, CG_FRAME_EVENT_SYNC, info);
}

void
_cg_onscreen_notify_complete(cg_onscreen_t *onscreen,
                             cg_frame_info_t *info)
{
    notify_event(onscreen, CG_FRAME_EVENT_COMPLETE, info);
}

void
_cg_onscreen_notify_resize(cg_onscreen_t *onscreen)
{
    cg_framebuffer_t *framebuffer = CG_FRAMEBUFFER(onscreen);

    _cg_closure_list_invoke(&onscreen->resize_closures,
                            cg_onscreen_resize_callback_t,
                            onscreen,
                            framebuffer->width,
                            framebuffer->height);
}

void
_cg_framebuffer_winsys_update_size(cg_framebuffer_t *framebuffer,
                                   int width,
                                   int height)
{
    if (framebuffer->width == width && framebuffer->height == height)
        return;

    framebuffer->width = width;
    framebuffer->height = height;

    cg_framebuffer_set_viewport(framebuffer, 0, 0, width, height);

    if (!_cg_has_private_feature(framebuffer->dev,
                                 CG_PRIVATE_FEATURE_DIRTY_EVENTS))
        _cg_onscreen_queue_full_dirty(CG_ONSCREEN(framebuffer));
}

void
cg_onscreen_set_resizable(cg_onscreen_t *onscreen, bool resizable)
{
    cg_framebuffer_t *framebuffer;
    const cg_winsys_vtable_t *winsys;

    if (onscreen->resizable == resizable)
        return;

    onscreen->resizable = resizable;

    framebuffer = CG_FRAMEBUFFER(onscreen);
    if (framebuffer->allocated) {
        winsys = _cg_framebuffer_get_winsys(CG_FRAMEBUFFER(onscreen));

        if (winsys->onscreen_set_resizable)
            winsys->onscreen_set_resizable(onscreen, resizable);
    }
}

bool
cg_onscreen_get_resizable(cg_onscreen_t *onscreen)
{
    return onscreen->resizable;
}

cg_onscreen_resize_closure_t *
cg_onscreen_add_resize_callback(cg_onscreen_t *onscreen,
                                cg_onscreen_resize_callback_t callback,
                                void *user_data,
                                cg_user_data_destroy_callback_t destroy)
{
    return _cg_closure_list_add(
        &onscreen->resize_closures, callback, user_data, destroy);
}

void
cg_onscreen_remove_resize_callback(cg_onscreen_t *onscreen,
                                   cg_onscreen_resize_closure_t *closure)
{
    _cg_closure_disconnect(closure);
}

cg_onscreen_dirty_closure_t *
cg_onscreen_add_dirty_callback(cg_onscreen_t *onscreen,
                               cg_onscreen_dirty_callback_t callback,
                               void *user_data,
                               cg_user_data_destroy_callback_t destroy)
{
    return _cg_closure_list_add(
        &onscreen->dirty_closures, callback, user_data, destroy);
}

void
cg_onscreen_remove_dirty_callback(cg_onscreen_t *onscreen,
                                  cg_onscreen_dirty_closure_t *closure)
{
    c_return_if_fail(closure);

    _cg_closure_disconnect(closure);
}

int64_t
cg_onscreen_get_frame_counter(cg_onscreen_t *onscreen)
{
    return onscreen->frame_counter;
}
