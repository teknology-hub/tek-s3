//===-- cm_callbacks.hpp - CM client callback declarations ----------------===//
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
/// Declarations of Steam CM client callback functions that may be used by
///    other modules.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "null_attrs.h" // IWYU pragma: keep

#include <tek-steamclient/cm.h>

namespace tek::s3 {

/// The callback for CM client connected event.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in] data
///    Pointer to `tek_sc_err` indicating the result of connection.
/// @param [in, out] user_data
///    Pointer to the @ref account object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_only, 2),
  access(read_write, 3)]]
void cb_connected(tek_sc_cm_client *_Nonnull client, void *_Nonnull data,
                  void *_Nonnull user_data);

/// The callback for CM client disconnected event.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in] data
///    Pointer to `tek_sc_err` indicating the disconnection reason.
/// @param [in, out] user_data
///    Pointer to the @ref account object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_only, 2),
  access(read_write, 3)]]
void cb_disconnected(tek_sc_cm_client *_Nonnull client, void *_Nonnull data,
                     void *_Nonnull user_data);

} // namespace tek::s3
