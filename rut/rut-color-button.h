/*
 * Rut
 *
 * Rig Utilities
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

#ifndef _RUT_COLOR_BUTTON_H_
#define _RUT_COLOR_BUTTON_H_

#include "rut-object.h"
#include "rut-context.h"

extern RutType rut_color_button_type;

typedef struct _RutColorButton RutColorButton;

RutColorButton *
rut_color_button_new (RutContext *ctx);

void
rut_color_button_set_color (RutObject *picker,
                            const CoglColor *color);

const CoglColor *
rut_color_button_get_color (RutColorButton *picker);

#endif /* _RUT_COLOR_BUTTON_H_ */
