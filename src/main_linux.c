//===-- main_linux.c - program entry point for GNU/Linux ------------------===//
//
// Copyright (c) 2025 Nuclearist <nuclearist@teknology-hub.com>
// Part of tek-s3, under the GNU General Public License v3.0 or later
// See https://github.com/teknology-hub/tek-s3/blob/main/COPYING for license
//    information.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//===----------------------------------------------------------------------===//
///
/// @file
/// Basic service skeleton for GNU/Linux systems.
///
//===----------------------------------------------------------------------===//

#include "config.h" // IWYU pragma: keep
#include "impl.h"

#include <locale.h>
#include <signal.h>
#include <stdlib.h>

#ifdef TEK_S3B_SYSTEMD
#include <systemd/sd-daemon.h>
#else
#define sd_notify(unset_environment, state)
#endif

/// Handler for `SIGINT` and `SIGTERM` signals.
static void ts3_sig_handler(int) {
  sd_notify(0, "STOPPING=1");
  ts3_stop();
}

int main(void) {
  setlocale(LC_ALL, "");
  if (ts3_init()) {
    const struct sigaction sa = {.sa_handler = ts3_sig_handler};
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
  } else {
    return EXIT_FAILURE;
  }
  sd_notify(0, "READY=1");
  ts3_run();
  return ts3_cleanup();
}
