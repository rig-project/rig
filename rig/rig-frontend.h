/*
 * Rut
 *
 * Copyright (C) 2013  Intel Corporation
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
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include <rut.h>

typedef struct _RigFrontend RigFrontend;

#include "rig-engine.h"


#ifndef _RIG_FRONTEND_H_
#define _RIG_FRONTEND_H_

/* The "frontend" is the main process that controls the running
 * of a Rig UI, either in device mode, as a slave or as an editor.
 *
 * Currently this is also the process where all rendering is handled,
 * though in the future more things may be split out into different
 * threads and processes.
 */
struct _RigFrontend
{
  RutObjectBase _base;


  RigEngine *engine;

  pid_t simulator_pid;

  int fd;
  RigRPCPeer *frontend_peer;

  bool has_resized;
  int pending_width;
  int pending_height;

};


RigFrontend *
rig_frontend_new (RutShell *shell,
                  const char *ui_filename);

void
rig_frontend_start_service (RigFrontend *frontend);

void
rig_frontend_stop_service (RigFrontend *frontend);

#endif /* _RIG_FRONTEND_H_ */