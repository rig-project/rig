/*
 * Rig
 *
 * UI Engine & Editor
 *
 * Copyright (C) 2013,2014  Intel Corporation.
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

#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>

#include <rut.h>

#include "rig-simulator.h"


int
main (int argc, char **argv)
{
  RigSimulator *simulator;
  const char *ipc_fd_str = getenv ("_RIG_IPC_FD");
  const char *frontend = getenv ("_RIG_FRONTEND");
  RigFrontendID frontend_id;

#ifdef unix
  sigset_t set;

  sigemptyset (&set);
  sigaddset (&set, SIGINT);

  /* Block SIGINT from terminating the simulator, otherwise it's a
   * pain to debug the frontend in gdb because when we press Ctrl-C to
   * interrupt the frontend, gdb only blocks SIGINT from being passed
   * to the frontend and so we end up terminating the simulator.
   */
  pthread_sigmask (SIG_BLOCK, &set, NULL);
#endif

#if 0
  GOptionContext *context = g_option_context_new (NULL);

  g_option_context_add_main_entries (context, rut_editor_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_error ("Option parsing failed: %s\n", error->message);
      return EXIT_FAILURE;
    }
#endif

  if (!ipc_fd_str)
    {
      g_error ("Failed to find ipc file descriptor via _RIG_IPC_FD "
               "environment variable");
      return EXIT_FAILURE;
    }

  if (!frontend)
    {
      g_error ("Failed to determine frontend via _RIG_FRONTEND "
               "environment variable");
      return EXIT_FAILURE;
    }

  if (strcmp (frontend, "editor") == 0)
    frontend_id = RIG_FRONTEND_ID_EDITOR;
  else if (strcmp (frontend, "slave") == 0)
    frontend_id = RIG_FRONTEND_ID_SLAVE;
  else if (strcmp (frontend, "device") == 0)
    frontend_id = RIG_FRONTEND_ID_DEVICE;
  else
    {
      g_error ("Spurious _RIG_FRONTEND environment variable value");
      return EXIT_FAILURE;
    }

  simulator = rig_simulator_new (frontend_id,
                                 strtol (ipc_fd_str, NULL, 10));

  rig_simulator_run (simulator);

  rut_object_unref (simulator);

  return 0;
}
