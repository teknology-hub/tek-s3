//===-- state.cpp - program state management functions --------------------===//
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
/// Implementation of @ref ts3_init, @ref ts3_stop and @ref ts3_cleanup.
///
//===----------------------------------------------------------------------===//
#include "state.hpp"

#include "cm_callbacks.hpp"
#include "config.h"
#include "impl.h"
#include "os.h"
#include "utils.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <libwebsockets.h>
#include <limits>
#include <memory>
#include <print>
#include <ranges>
#include <rapidjson/document.h>
#include <rapidjson/reader.h>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tek-steamclient/base.h>
#include <tek-steamclient/os.h>
#include <utility>

namespace tek::s3 {

namespace {

//===-- Private variable --------------------------------------------------===//

/// permessage-deflate WebSocket extension object.
static constexpr lws_extension ws_pm_ext[]{
    {.name = "permessage-deflate",
     .callback = lws_extension_callback_pm_deflate,
     .client_offer = "client_no_context_takeover; server_no_context_takeover; "
                     "client_max_window_bits"},
    {}};

/// libwebsockets HTTP mount object.
static constexpr lws_http_mount mount{.mount_next = nullptr,
                                      .mountpoint = "/",
                                      .origin = nullptr,
                                      .def = nullptr,
                                      .protocol = "tek-s3",
                                      .cgienv = nullptr,
                                      .extra_mimetypes = nullptr,
                                      .interpret = nullptr,
                                      .cgi_timeout = 0,
                                      .cache_max_age = 0,
                                      .auth_mask = 0,
                                      .cache_reusable = 0,
                                      .cache_revalidate = 0,
                                      .cache_intermediaries = 0,
                                      .origin_protocol = LWSMPRO_CALLBACK,
                                      .mountpoint_len = 1,
                                      .basic_auth_login_file = nullptr};

//===-- Private function --------------------------------------------------===//

/// Print an OS error message to stderr.
///
/// @param errc
///    OS error code.
/// @param [in] msg
///    Program-defined message identifying which operation failed.
static inline void print_os_err(tek_sc_os_errc errc,
                                const std::string_view &&msg) {
  const auto err_msg = ts3_os_get_err_msg(errc);
  std::println(std::cerr, "{}: ({}) {}", msg, errc, err_msg);
  std::free(err_msg);
}

} // namespace

//===-- Internal functions ------------------------------------------------===//

extern "C" {

bool ts3_init(void) {
  std::println("tek-s3 " TEK_S3_VERSION);
  // Initialize tek-steamclient library context
  std::unique_ptr<tek_sc_lib_ctx, decltype(&tek_sc_lib_cleanup)> tek_sc_ctx(
      tek_sc_lib_init(true, true), tek_sc_lib_cleanup);
  if (!tek_sc_ctx) {
    std::println(std::cerr, "tek_sc_lib_init failed");
    return false;
  }
  // Load state
  {
    const auto state_dir = ts3_os_get_state_dir();
    if (!state_dir) {
      std::println("State directory not found, initializing new state");
      goto skip_state_file;
    }
    auto state_file_path = std::basic_string<tek_sc_os_char>(state_dir).append(
        TEK_SC_OS_STR("" TS3_OS_PATH_SEP_CHAR_STR
                      "tek-s3" TS3_OS_PATH_SEP_CHAR_STR "state.json"));
    std::free(state_dir);
    os_handle state_file_handle(ts3_os_file_open(state_file_path.data()));
    if (!state_file_handle) {
      if (const auto errc = ts3_os_get_last_error();
          errc != TS3_OS_ERR_FILE_NOT_FOUND) {
        print_os_err(errc, "Failed to open state file");
        return false;
      }
      std::println("State file not found, initializing new state");
      goto skip_state_file;
    }
    state_file_path = {};
    const auto state_file_size = ts3_os_file_get_size(state_file_handle.value);
    if (state_file_size == std::numeric_limits<std::size_t>::max()) {
      print_os_err(ts3_os_get_last_error(), "Failed to get state file size");
      return false;
    }
    const auto state_file_data =
        std::make_unique_for_overwrite<char[]>(state_file_size + 1);
    if (!ts3_os_file_read(state_file_handle.value, state_file_data.get(),
                          state_file_size)) {
      print_os_err(ts3_os_get_last_error(), "Failed to read state file");
      return false;
    }
    state_file_handle.close();
    // Null-terminate the buffer for in-situ parsing to work correctly
    state_file_data[state_file_size] = '\0';
    rapidjson::Document doc;
    doc.ParseInsitu<rapidjson::kParseStopWhenDoneFlag>(state_file_data.get());
    if (doc.HasParseError() || !doc.IsObject()) {
      std::println(std::cerr, "Failed to parse state file's JSON");
      return false;
    }
    if (const auto timestamp = doc.FindMember("timestamp");
        timestamp != doc.MemberEnd() && timestamp->value.IsUint64()) {
      state.timestamp = static_cast<std::time_t>(timestamp->value.GetUint64());
    }
    if (const auto accounts = doc.FindMember("accounts");
        accounts != doc.MemberEnd() && accounts->value.IsArray()) {
      for (const auto now = std::chrono::system_clock::to_time_t(
               std::chrono::system_clock::now());
           const auto &acc : accounts->value.GetArray()) {
        if (!acc.IsString()) {
          continue;
        }
        const std::string token(acc.GetString(), acc.GetStringLength());
        const auto token_info = tek_sc_cm_parse_auth_token(token.data());
        if (!token_info.steam_id) {
          std::println(std::cerr, "Auth token \"{}\" is invalid; skipping it",
                       token);
          continue;
        }
        if (token_info.expires < now) {
          std::println(std::cerr,
                       "Auth token for account {} has expired; skipping it",
                       token_info.steam_id);
          continue;
        }
        state.accounts.try_emplace(
            token_info.steam_id, lws_sorted_usec_list_t{}, nullptr,
            std::move(token), token_info, renew_status::not_scheduled, 0, 0,
            remove_status::none, std::unique_ptr<tek_sc_cm_data_depot_key[]>{},
            std::set<std::uint32_t>{});
      }
    }
    if (const auto apps = doc.FindMember("apps");
        apps != doc.MemberEnd() && apps->value.IsObject()) {
      for (const auto &[id, depots] : apps->value.GetObject()) {
        if (!depots.IsArray()) {
          continue;
        }
        std::uint32_t app_id;
        if (const std::string_view view(id.GetString(), id.GetStringLength());
            std::from_chars(view.begin(), view.end(), app_id).ec !=
            std::errc{}) {
          continue;
        }
        for (auto &app = state.apps.try_emplace(app_id).first->second;
             const auto &depot_id : depots.GetArray()) {
          if (!depot_id.IsUint()) {
            continue;
          }
          app.depots.emplace(static_cast<std::uint32_t>(depot_id.GetUint()),
                             depot{});
        }
      }
    }
    if (const auto depot_keys = doc.FindMember("depot_keys");
        depot_keys != doc.MemberEnd() && depot_keys->value.IsObject()) {
      for (const auto &[id, b64_key] : depot_keys->value.GetObject()) {
        std::uint32_t depot_id;
        if (const std::string_view view(id.GetString(), id.GetStringLength());
            std::from_chars(view.begin(), view.end(), depot_id).ec !=
            std::errc{}) {
          continue;
        }
        if (!b64_key.IsString() || b64_key.GetStringLength() != 44) {
          continue;
        }
        ts3_u_base64_decode(b64_key.GetString(), 44,
                            state.depot_keys[depot_id]);
      }
    }
  } // State file loading scope
skip_state_file:
  // Load settings
  std::string endpoint;
  {
    const auto config_dir = ts3_os_get_config_dir();
    if (!config_dir) {
      std::println("Config directory not found, using defaults");
      goto skip_settings_file;
    }
    auto settings_file_path =
        std::basic_string<tek_sc_os_char>(config_dir)
            .append(TEK_SC_OS_STR("" TS3_OS_PATH_SEP_CHAR_STR
                                  "tek-s3" TS3_OS_PATH_SEP_CHAR_STR
                                  "settings.json"));
    std::free(config_dir);
    os_handle settings_file_handle(ts3_os_file_open(settings_file_path.data()));
    if (!settings_file_handle) {
      if (const auto errc = ts3_os_get_last_error();
          errc != TS3_OS_ERR_FILE_NOT_FOUND) {
        print_os_err(errc, "Failed to open settings file");
        return false;
      }
      std::println("Settings file not found, using defaults");
      goto skip_settings_file;
    }
    settings_file_path = {};
    const auto settings_file_size =
        ts3_os_file_get_size(settings_file_handle.value);
    if (settings_file_size == std::numeric_limits<std::size_t>::max()) {
      print_os_err(ts3_os_get_last_error(), "Failed to get settings file size");
      return false;
    }
    const auto settings_file_data =
        std::make_unique_for_overwrite<char[]>(settings_file_size + 1);
    if (!ts3_os_file_read(settings_file_handle.value, settings_file_data.get(),
                          settings_file_size)) {
      print_os_err(ts3_os_get_last_error(), "Failed to read settings file");
      return false;
    }
    settings_file_handle.close();
    // Null-terminate the buffer for in-situ parsing to work correctly
    settings_file_data[settings_file_size] = '\0';
    rapidjson::Document doc;
    doc.ParseInsitu<rapidjson::kParseStopWhenDoneFlag>(
        settings_file_data.get());
    if (doc.HasParseError() || !doc.IsObject()) {
      std::println(std::cerr, "Failed to parse settings file's JSON");
      return false;
    }
    const auto listen_endpoint = doc.FindMember("listen_endpoint");
    if (listen_endpoint != doc.MemberEnd() &&
        listen_endpoint->value.IsString()) {
      endpoint = {listen_endpoint->value.GetString(),
                  listen_endpoint->value.GetStringLength()};
    }
  } // Settings file loading scope
skip_settings_file:
  // Parse listen_endpoint
  const char *iface;
  int port;
#ifdef __linux__
  const char *uds_perms = nullptr;
#endif // __linux__
  if (endpoint.empty()) {
    iface = "127.0.0.1";
    port = 8080;
  } else {
#ifdef __linux__
    if (endpoint.starts_with("unix:")) {
      iface = "/run/tek-s3.sock";
      port = 0;
      uds_perms = &endpoint[5];
    } else
#endif // __linux__
    {
      const auto colon_pos = endpoint.rfind(':');
      if (colon_pos == std::string::npos) {
        std::println(std::cerr, "Invalid listen_endpoint value: ':' not found");
        return false;
      }
      endpoint[colon_pos] = '\0';
      iface = endpoint.data();
      if (const std::string_view port_view(endpoint.begin() + colon_pos + 1,
                                           endpoint.end());
          std::from_chars(port_view.begin(), port_view.end(), port).ec !=
          std::errc{}) {
        std::println(std::cerr,
                     "Invalid listen_endpoint value: invalid port number");
        return false;
      }
      if (port < 1 || port > 65535) {
        std::println(std::cerr,
                     "Invalid listen_endpoint value: port number must be in "
                     "range [1, 65535]");
        return false;
      }
    }
  }
  // Create libwebsockets context
  std::unique_ptr<lws_context, decltype(&lws_context_destroy)> lws_ctx(
      nullptr, lws_context_destroy);
  {
    lws_context_creation_info info{};
    info.iface = iface;
    info.extensions = ws_pm_ext;
    info.port = port;
    info.timeout_secs = 10;
    // Try to use libuv for better event loop performance
    info.options =
        LWS_SERVER_OPTION_LIBUV | LWS_SERVER_OPTION_FAIL_UPON_UNABLE_TO_BIND;
#ifdef __linux__
    if (uds_perms) {
      info.options |= LWS_SERVER_OPTION_UNIX_SOCK;
      info.unix_socket_perms = uds_perms;
    }
#endif // __linux__
    const lws_protocols *pprotocols[]{&protocol, nullptr};
    info.pprotocols = pprotocols;
    info.mounts = &mount;
    lws_ctx.reset(lws_create_context(&info));
    if (!lws_ctx) {
      // Probably libwebsockets was compiled without libuv support, try
      //    disabling it
      info.options &= ~LWS_SERVER_OPTION_LIBUV;
      lws_ctx.reset(lws_create_context(&info));
      if (!lws_ctx) {
        // And this is a real failure
        std::println(std::cerr, "lws_create_context failed");
        return false;
      }
    }
  } // lws_ctx initialization scope
  // Create CM client instances
  for (auto it = state.accounts.begin(); it != state.accounts.end(); ++it) {
    auto &acc = it->second;
    acc.cm_client = tek_sc_cm_client_create(tek_sc_ctx.get(), &acc);
    if (!acc.cm_client) {
      for (auto cm_client : std::ranges::subrange(state.accounts.begin(), it) |
                                std::views::values |
                                std::views::transform(&account::cm_client)) {
        tek_sc_cm_client_destroy(cm_client);
      }
      std::println(std::cerr, "tek_sc_cm_client_create failed");
      return false;
    }
  }
  state.lws_ctx = lws_ctx.release();
  state.tek_sc_ctx = tek_sc_ctx.release();
  // Connect CM clients or update the manifest if there are none
  if (state.accounts.empty()) {
    if (!state.apps.empty()) {
      state.apps.clear();
      state.manifest_dirty = true;
    }
    update_manifest();
    state.cur_status.store(status::running, std::memory_order::relaxed);
  } else {
    for (auto cm_client : state.accounts | std::views::values |
                              std::views::transform(&account::cm_client)) {
      tek_sc_cm_connect(cm_client, cb_connected, 5000, cb_disconnected);
    }
  }
  return true;
}

void ts3_stop(void) {
  state.cur_status.store(status::stopping, std::memory_order::relaxed);
  lws_cancel_service(state.lws_ctx);
}

int ts3_cleanup(void) {
  for (auto cm_client : state.accounts | std::views::values |
                            std::views::transform(&account::cm_client)) {
    tek_sc_cm_client_destroy(cm_client);
  }
  for (;;) {
    const auto cur_num_conns =
        state.num_cm_connections.load(std::memory_order::relaxed);
    if (!cur_num_conns) {
      break;
    }
    ts3_os_futex_wait(&state.num_cm_connections, cur_num_conns,
                      std::numeric_limits<std::uint32_t>::max());
  }
  tek_sc_lib_cleanup(state.tek_sc_ctx);
  return state.exit_code;
}

} // extern "C"

} // namespace tek::s3
