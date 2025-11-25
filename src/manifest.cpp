//===-- manifest.cpp - manifest update implementation ---------------------===//
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
/// Implementation of @ref update_manifest.
///
//===----------------------------------------------------------------------===//
#include "state.hpp"

#include "config.h" // IWYU pragma: keep
#include "os.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <print>
#include <ranges>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <span>
#include <string_view>
#include <system_error>
#include <tek-steamclient/os.h>
#include <utility>
#ifdef TEK_S3B_ZNG
#include <zlib-ng.h>
#else // def TEK_S3B_ZNG
#include <zlib.h>
#endif // def TEK_S3B_ZNG else

namespace tek::s3 {

namespace {

//===-- Binary manifest types ---------------------------------------------===//
//
// The structure of binary manifest is as following:
//    bmanifest_hdr
//    bmanifest_app[num_apps]
//    std::uint32_t[num_depots]
//    bmanifest_depot_key[num_depot_keys]
//    char names[*the remainder of buffer*]

/// Binary manifest header.
struct bmanifest_hdr {
  /// CRC32 checksum for the remainder of serialized data (excluding itself).
  std::uint32_t crc;
  /// Total number of application entries in the manifest.
  std::int32_t num_apps;
  /// Total number of depot entries in the manifest.
  std::int32_t num_depots;
  /// Total number of depot decryption key entries in the manifest.
  std::int32_t num_depot_keys;
};

/// Binary manifest application entry.
struct bmanifest_app {
  /// PICS access token for the application.
  std::uint64_t pics_access_token;
  /// Length of the application's name, in bytes.
  std::int32_t name_len;
  /// Number of depot IDs assigned to the application.
  std::int32_t num_depots;
};

/// Binary manifest depot decryption key entry.
struct bmanifest_depot_key {
  /// ID of the depot.
  std::int32_t id;
  /// Decryption key for the depot.
  tek_sc_aes256_key key;
};

//===-- Private functions -------------------------------------------------===//

#ifdef TEK_S3B_ZNG
static constexpr auto &crc32{::zng_crc32};
#else  // def TEK_S3B_ZNG
static constexpr auto &crc32{::crc32};
#endif // def TEK_S3B_ZNG else

/// Print an OS error message to stderr.
///
/// @param errc
///    OS error code.
/// @param [in] msg
///    Program-defined message identifying which operation failed.
static inline void print_os_err(tek_sc_os_errc errc,
                                const std::string_view &&msg) {
  const auto err_msg{ts3_os_get_err_msg(errc)};
  std::println(std::cerr, "{}: ({}) {}", msg, errc, err_msg);
  std::free(err_msg);
}

} // namespace

void update_manifest() {
  rapidjson::StringBuffer buf;
  if (state.manifest_dirty || !state.manifest.buf.size) {
    if (state.manifest_dirty) {
      state.state_dirty = true;
      state.manifest_dirty = false;
      state.timestamp = std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now());
    }
    // Serialize JSON manifest
    rapidjson::Writer writer{buf};
    writer.StartObject();
    std::string_view str{"apps"};
    writer.Key(str.data(), str.length());
    writer.StartObject();
    for (const auto &[app_id, app] : state.apps) {
      std::array<char, 10> id_buf;
      const auto res{std::to_chars(id_buf.begin(), id_buf.end(), app_id)};
      if (res.ec != std::errc{}) {
        continue;
      }
      str = {id_buf.data(), res.ptr};
      writer.Key(str.data(), str.length());
      writer.StartObject();
      str = "name";
      writer.Key(str.data(), str.length());
      writer.String(app.name.data(), app.name.length());
      if (app.pics_access_token) {
        str = "pics_at";
        writer.Key(str.data(), str.length());
        writer.Uint64(app.pics_access_token);
      }
      str = "depots";
      writer.Key(str.data(), str.length());
      writer.StartArray();
      for (auto depot_id : app.depots | std::views::keys) {
        writer.Uint(depot_id);
      }
      writer.EndArray();
      writer.EndObject();
    }
    writer.EndObject();
    str = "depot_keys";
    writer.Key(str.data(), str.length());
    writer.StartObject();
    for (const auto &[depot_id, key] : state.depot_keys) {
      std::array<char, 10> id_buf;
      const auto res = std::to_chars(id_buf.begin(), id_buf.end(), depot_id);
      if (res.ec != std::errc{}) {
        continue;
      }
      str = {id_buf.data(), res.ptr};
      writer.Key(str.data(), str.length());
      std::array<char, 44> b64_key;
      ts3_u_base64_encode(key, sizeof key, b64_key.data());
      writer.String(b64_key.data(), b64_key.size());
    }
    writer.EndObject();
    writer.EndObject();
    // Copy serialized JSON into a buffer
    sized_buf json_buf{
        .buf = std::make_unique_for_overwrite<unsigned char[]>(buf.GetSize()),
        .size = buf.GetSize()};
    std::ranges::copy_n(buf.GetString(), json_buf.size, json_buf.buf.get());
    state.manifest = {std::move(json_buf), false};
    buf.Clear();
    // Serialize binary manifest
    for (auto &a : state.apps) {
      a.second.depots.size();
    }
    std::size_t num_depots = 0;
    std::size_t names_len = 0;
    for (const auto &app : state.apps | std::views::values) {
      num_depots += app.depots.size();
      names_len += app.name.length();
    }
    const auto bmanifest_size{
        sizeof(bmanifest_hdr) + sizeof(bmanifest_app) * state.apps.size() +
        sizeof(std::uint32_t) * num_depots +
        sizeof(bmanifest_depot_key) * state.depot_keys.size() + names_len};
    auto bmanifest{
        std::make_unique_for_overwrite<unsigned char[]>(bmanifest_size)};
    auto &hdr{*reinterpret_cast<bmanifest_hdr *>(bmanifest.get())};
    hdr.num_apps = state.apps.size();
    hdr.num_depots = num_depots;
    hdr.num_depot_keys = state.depot_keys.size();
    std::span<bmanifest_app> bapps{reinterpret_cast<bmanifest_app *>(&hdr + 1),
                                   state.apps.size()};
    std::span<std::uint32_t> depots{
        reinterpret_cast<std::uint32_t *>(std::to_address(bapps.end())),
        num_depots};
    std::span<bmanifest_depot_key> bdepot_keys{
        reinterpret_cast<bmanifest_depot_key *>(std::to_address(depots.end())),
        state.depot_keys.size()};
    auto names{reinterpret_cast<char *>(std::to_address(bdepot_keys.end()))};
    auto depot_it{depots.begin()};
    for (auto &&[bapp, app] :
         std::views::zip(bapps, state.apps | std::views::values)) {
      bapp = {.pics_access_token = app.pics_access_token,
              .name_len = static_cast<std::int32_t>(app.name.length()),
              .num_depots = static_cast<std::int32_t>(app.depots.size())};
      names = std::ranges::copy(app.name, names).out;
      depot_it = std::ranges::copy(app.depots | std::views::keys, depot_it).out;
    }
    for (auto &&[bdk, dk] : std::views::zip(bdepot_keys, state.depot_keys)) {
      bdk.id = dk.first;
      std::ranges::copy(dk.second, bdk.key);
    }
    hdr.crc = crc32(crc32(0, nullptr, 0), &bmanifest[sizeof hdr.crc],
                    bmanifest_size - sizeof hdr.crc);
    state.manifest_bin = {{std::move(bmanifest), bmanifest_size}, true};
  } // if (state.manifest_dirty || !state.manifest.buf.size)
  // Update the state file if it's marked dirty
  if (state.state_dirty) {
    state.state_dirty = false;
    // Serialize the state into JSON
    rapidjson::Writer writer{buf};
    writer.StartObject();
    std::string_view str{"timestamp"};
    writer.Key(str.data(), str.length());
    writer.Uint64(state.timestamp);
    str = "accounts";
    writer.Key(str.data(), str.length());
    writer.StartArray();
    for (const auto &acc : state.accounts | std::views::values) {
      if (acc.rem_status == remove_status::none) {
        writer.String(acc.token.data(), acc.token.length());
      }
    }
    writer.EndArray();
    str = "apps";
    writer.Key(str.data(), str.length());
    writer.StartObject();
    for (const auto &[app_id, app] : state.apps) {
      std::array<char, 10> id_buf;
      const auto res{std::to_chars(id_buf.begin(), id_buf.end(), app_id)};
      if (res.ec != std::errc{}) {
        continue;
      }
      str = {id_buf.data(), res.ptr};
      writer.Key(str.data(), str.length());
      writer.StartObject();
      if (app.pics_access_token) {
        str = "pics_at";
        writer.Key(str.data(), str.length());
        writer.Uint64(app.pics_access_token);
      }
      str = "depots";
      writer.Key(str.data(), str.length());
      writer.StartArray();
      for (auto depot_id : app.depots | std::views::keys) {
        writer.Uint(depot_id);
      }
      writer.EndArray();
      writer.EndObject();
    }
    writer.EndObject();
    str = "depot_keys";
    writer.Key(str.data(), str.length());
    writer.StartObject();
    for (const auto &[depot_id, key] : state.depot_keys) {
      std::array<char, 10> id_buf;
      const auto res{std::to_chars(id_buf.begin(), id_buf.end(), depot_id)};
      if (res.ec != std::errc{}) {
        continue;
      }
      str = {id_buf.data(), res.ptr};
      writer.Key(str.data(), str.length());
      std::array<char, 44> b64_key;
      ts3_u_base64_encode(key, sizeof key, b64_key.data());
      writer.String(b64_key.data(), b64_key.size());
    }
    writer.EndObject();
    writer.EndObject();
    // Write serialized JSON into the file
    std::unique_ptr<tek_sc_os_char[], decltype(&std::free)> state_dir{
        ts3_os_get_state_dir(), std::free};
    if (!state_dir) {
      std::println(std::cerr, "Cannot save state: state directory not found");
      return;
    }
    os_handle state_dir_handle{ts3_os_dir_create(state_dir.get())};
    if (!state_dir_handle) {
      print_os_err(ts3_os_get_last_error(),
                   "Cannot save state; failed to open state directory");
      return;
    }
    state_dir.reset();
    os_handle ts3_dir_handle{
        ts3_os_dir_create_at(state_dir_handle.value, TEK_SC_OS_STR("tek-s3"))};
    if (!ts3_dir_handle) {
      print_os_err(ts3_os_get_last_error(),
                   "Cannot save state; failed to open tek-s3 subdirectory");
      return;
    }
    state_dir_handle.close();
    os_handle state_file_handle{ts3_os_file_create_at(
        ts3_dir_handle.value, TEK_SC_OS_STR("state.json"))};
    if (!state_file_handle) {
      print_os_err(ts3_os_get_last_error(),
                   "Cannot save state; failed to open state file");
      return;
    }
    ts3_dir_handle.close();
    if (!ts3_os_file_write(state_file_handle.value, buf.GetString(),
                           buf.GetSize())) {
      print_os_err(ts3_os_get_last_error(),
                   "Cannot save state; failed to write to the state file");
    }
  } // if (state.state_dirty)
}

} // namespace tek::s3
