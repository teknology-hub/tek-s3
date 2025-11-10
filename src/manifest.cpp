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
#include <cstdlib>
#include <iostream>
#include <print>
#include <ranges>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string_view>
#include <system_error>
#include <tek-steamclient/os.h>
#include <utility>
#ifdef TEK_S3B_ZNG
#include <zlib-ng.h>
#else // def TEK_S3B_ZNG
#include <zlib.h>
#endif // def TEK_S3B_ZNG else
#ifdef TEK_S3B_BROTLI
#include <brotli/encode.h>
#endif // def TEK_S3B_BROTLI
#ifdef TEK_S3B_ZSTD
#include <zstd.h>
#endif // def TEK_S3B_ZSTD

namespace tek::s3 {

namespace {

#ifdef TEK_S3B_ZNG

static constexpr auto &compressBound{::zng_compressBound};
static constexpr auto &compress2{::zng_compress2};

#else // def TEK_S3B_ZNG

static constexpr auto compressBound{::compressBound};
static constexpr auto compress2{::compress2};

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
  if (state.manifest_dirty || !state.manifest.size) {
    if (state.manifest_dirty) {
      state.state_dirty = true;
      state.manifest_dirty = false;
      state.timestamp = std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now());
    }
    // Serialize manifest into JSON
    {
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
      // Copy serialized JSON into the manifest buffer
      state.manifest.buf.reset(new unsigned char[buf.GetSize()]);
      state.manifest.size = buf.GetSize();
      std::ranges::copy_n(buf.GetString(), buf.GetSize(),
                          state.manifest.buf.get());
    } // Manifest JSON serialization scope
    buf.Clear();
    // Deflate the manifest
    {
      const auto worst_size{compressBound(state.manifest.size)};
      auto tmp_buf{std::make_unique_for_overwrite<unsigned char[]>(worst_size)};
      state.manifest_deflate.size = worst_size;
#ifdef TEK_S3B_ZNG
      std::size_t size;
#else  // def TEK_S3B_ZNG
      uLongf size;
#endif // def TEK_S3B_ZNG else
      const auto res{compress2(tmp_buf.get(), &size, state.manifest.buf.get(),
                               state.manifest.size, Z_BEST_COMPRESSION)};
      state.manifest_deflate.size = size;
      if (res != Z_OK) {
        state.manifest_deflate.buf.reset();
        state.manifest_deflate.size = 0;
      } else if (state.manifest_deflate.size == worst_size) {
        state.manifest_deflate.buf = std::move(tmp_buf);
      } else {
        state.manifest_deflate.buf.reset(
            new unsigned char[state.manifest_deflate.size]);
        std::ranges::copy_n(tmp_buf.get(), state.manifest_deflate.size,
                            state.manifest_deflate.buf.get());
      }
    }
#ifdef TEK_S3B_BROTLI
    // Compress the manifest with brotli
    {
      const auto worst_size{
          BrotliEncoderMaxCompressedSize(state.manifest.size)};
      auto tmp_buf{std::make_unique_for_overwrite<unsigned char[]>(worst_size)};
      state.manifest_brotli.size = worst_size;
      const int res{BrotliEncoderCompress(
          BROTLI_MAX_QUALITY, BROTLI_MAX_WINDOW_BITS, BROTLI_MODE_TEXT,
          state.manifest.size, state.manifest.buf.get(),
          &state.manifest_brotli.size, tmp_buf.get())};
      if (res == BROTLI_FALSE) {
        state.manifest_brotli.buf.reset();
        state.manifest_brotli.size = 0;
      } else if (state.manifest_brotli.size == worst_size) {
        state.manifest_brotli.buf = std::move(tmp_buf);
      } else {
        state.manifest_brotli.buf.reset(
            new unsigned char[state.manifest_brotli.size]);
        std::ranges::copy_n(tmp_buf.get(), state.manifest_brotli.size,
                            state.manifest_brotli.buf.get());
      }
    }
#endif // def TEK_S3B_BROTLI
#ifdef TEK_S3B_ZSTD
    // Compress the manifest with zstd
    {
      const auto worst_size{ZSTD_compressBound(state.manifest.size)};
      auto tmp_buf{std::make_unique_for_overwrite<unsigned char[]>(worst_size)};
      state.manifest_zstd.size =
          ZSTD_compress(tmp_buf.get(), worst_size, state.manifest.buf.get(),
                        state.manifest.size, ZSTD_maxCLevel());
      if (ZSTD_isError(state.manifest_zstd.size)) {
        state.manifest_zstd.buf.reset();
        state.manifest_zstd.size = 0;
      } else if (state.manifest_zstd.size == worst_size) {
        state.manifest_zstd.buf = std::move(tmp_buf);
      } else {
        state.manifest_zstd.buf.reset(
            new unsigned char[state.manifest_zstd.size]);
        std::ranges::copy_n(tmp_buf.get(), state.manifest_zstd.size,
                            state.manifest_zstd.buf.get());
      }
    }
#endif // def TEK_S3B_ZSTD
  } // Manifest update scope
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
      writer.StartArray();
      for (auto depot_id : app.depots | std::views::keys) {
        writer.Uint(depot_id);
      }
      writer.EndArray();
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
  } // State file update scope
}

} // namespace tek::s3
