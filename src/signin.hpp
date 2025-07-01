//===-- signin.hpp - Steam account sign-in handling -----------------------===//
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
/// Declarations of context types and message processing function for sign-in.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "null_attrs.h" // IWYU pragma: keep

#include <cstddef>
#include <libwebsockets.h>
#include <memory>
#include <mutex>
#include <string>
#include <tek-steamclient/cm.h>

namespace tek::s3 {

/// Steam authentication types.
enum class auth_type {
  /// Credentials-based authentication.
  credentials,
  /// QR code-based authentication.
  qr
};

/// Sign-in states that determine which incoming messages are expected.
enum class signin_state {
  /// Awaiting initial client message with sign-in type, and credentials for
  ///    credentials-based authentication.
  awaiting_init,
  /// Awaiting response from Steam CM server, no incoming messages are accepted.
  awaiting_cm_response,
  /// Awaiting sign-in confirmation from the client.
  awaiting_confirmation,
  /// Authentication is complete, the connection should be closed.
  done,
  /// The CM client instance has been disconnected.
  disonnected
};

/// Steam sign-in context.
struct signin_ctx {
  /// Pointer to the CM client instance performing the sign-in.
  std::unique_ptr<tek_sc_cm_client, decltype(&tek_sc_cm_client_destroy)>
      cm_client;
  /// Pointer to the client WebSocket connection instance.
  lws *_Nonnull wsi;
  /// Current sign-in state.
  signin_state state;
  /// Selected authentication type.
  auth_type type;
  /// Pointer to the buffer for outgoing messages.
  unsigned char *_Nonnull msg_buf;
  /// Size of the message to send, in bytes. Value of `0` indicates that there
  ///    is no outgoing message pending, and value of `-1` indicates that
  ///    connection should be closed.
  int msg_size;
  /// Mutex for locking concurrent access to context fields.
  std::recursive_mutex mtx;
  /// Steam account name for credentials-based authentication.
  std::string account_name;
  /// Steam account password for credentials-based authentication.
  std::string password;
  /// On success, Steam authentication token.
  std::string token;
};

/// Process incoming WebSocket message.
///
/// @param [in, out] ctx
///    Sign-in context.
/// @param [in, out] msg
///    Pointer to the buffer containing the message text.
/// @param msg_size
///    Size of the message, in bytes.
/// @return `0` on success, `1` if connection must be closed.
[[using gnu: visibility("internal"), nonnull(2), access(read_write, 2, 3)]]
int signin_process_msg(signin_ctx &ctx, char *_Nonnull msg,
                       std::size_t msg_size);

} // namespace tek::s3
