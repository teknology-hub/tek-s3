//===-- impl.h - Primary program function declarations --------------------===//
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
/// Declarations of primary functions to be called from the main module.
///
//===----------------------------------------------------------------------===//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif // def __cplusplus

/// Load program settings and setup the global state.
///
/// @return Value indicating whether initialization succeeded.
[[gnu::visibility("internal")]]
bool ts3_init(void);

/// Run the server and block until it stops.
[[gnu::visibility("internal")]]
void ts3_run(void);

/// Request the server to stop and run @ref ts3_cleanup afterwards.
[[gnu::visibility("internal")]]
void ts3_stop(void);

/// Free all resources used by the program.
///
/// @return The exit code for the process.
[[gnu::visibility("internal")]]
int ts3_cleanup(void);

#ifdef __cplusplus
} // extern "C"
#endif // def __cplusplus
