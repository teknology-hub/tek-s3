//===-- server.cpp - HTTP/WS server implementation ------------------------===//
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
/// Implementation of libwebsockets protocol callbacks and @ref ts3_run.
///
//===----------------------------------------------------------------------===//
#include "impl.h"

#include "config.h"     // IWYU pragma: keep
#include "null_attrs.h" // IWYU pragma: keep
#include "os.h"
#include "signin.hpp"
#include "state.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <iomanip>
#include <iterator>
#include <libwebsockets.h>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>
#include <tek-steamclient/cm.h>
#include <tek-steamclient/error.h>

namespace tek::s3 {

namespace {

#ifdef _WIN32
static constexpr auto &timegm{::_mkgmtime};
#endif // _WIN32

//===-- Private types -----------------------------------------------------===//

/// Possible encoding types for manifest responses.
enum class enc_type {
  none,
  deflate,
#ifdef TEK_S3B_BROTLI
  brotli,
#endif // def TEK_S3B_BROTLI
#ifdef TEK_S3B_ZSTD
  zstd
#endif // def TEK_S3B_ZSTD
};

/// Manifest request code await entry.
struct mrc_await_entry {
  /// Request/response data for tek-steamclient.
  tek_sc_cm_data_mrc data;
  /// Value that is set to `1` and signaled when the callback is received.
  std::atomic_uint32_t finished;
};

/// Per-session context for HTTP sessions.
struct http_ctx {
  /// Next chunk of data to send.
  std::span<unsigned char> data;
  /// Send buffer.
  std::array<unsigned char, tx_size> tx_buf;
};

/// Per-session context for WebSocket sessions.
struct ws_ctx {
  /// Sign-in context.
  std::unique_ptr<signin_ctx> s_ctx;
  /// Send buffer.
  std::array<unsigned char, tx_size> tx_buf;
};

//===-- Private functions -------------------------------------------------===//

/// The callback for CM client manifest request code received event.
///
/// @param [in, out] data
///    Pointer to `mrc_await_entry` associated with the request.
[[using gnu: nonnull(2), access(read_write, 2)]]
static void cb_mrc(tek_sc_cm_client *, void *_Nonnull data, void *) {
  auto &entry{*reinterpret_cast<mrc_await_entry *>(data)};
  entry.finished.store(1, std::memory_order::relaxed);
  ts3_os_futex_wake(&entry.finished);
}

/// Select response encoding based on which encodings are supported by the
///    client, and sizes of manifest data in each supported one.
///
/// @param [in] accept
///    Value of the Accept-Encoding header sent by the client.
/// @param [in] buf
///    Buffer that will be sent back to the client.
/// @return Value indicating which encoding shall be used.
static constexpr enc_type negotiate_enc(const std::string_view &&accept,
                                        const http_buf &buf) noexcept {
  if (accept.empty()) {
    return enc_type::none;
  }
  auto enc{enc_type::none};
  auto size{buf.buf.size};
  if (buf.deflate.buf && accept.contains("deflate") &&
      buf.deflate.size < size) {
    enc = enc_type::deflate;
    size = buf.deflate.size;
  }
#ifdef TEK_S3B_BROTLI
  if (buf.brotli.buf && accept.contains("br") && buf.brotli.size < size) {
    enc = enc_type::brotli;
    size = buf.brotli.size;
  }
#endif // def TEK_S3B_BROTLI
#ifdef TEK_S3B_ZSTD
  if (buf.zstd.buf && accept.contains("zstd") && buf.zstd.size < size) {
    enc = enc_type::zstd;
    size = buf.zstd.size;
  }
#endif // def TEK_S3B_ZSTD
  return enc;
}

/// Remove a manifest request code cache entry.
///
/// @param [in, out] sul
///    Pointer to the scheduling element.
[[using gnu: nonnull(1), access(read_only, 1)]]
static void remove_mrc_cache(lws_sorted_usec_list_t *_Nonnull sul) {
  state.mrcs.erase(reinterpret_cast<const mrc_cache *>(sul)->manifest_id);
}

// Process a libwebsockets protocol callback.
///
/// @param wsi
///    Pointer to the WebSocket instance that emitted the callback.
/// @param reason
///    Reason for the callback.
/// @param in
///    For wsi-scoped callbacks, pointer to the associated context.
/// @param [in] in
///    Pointer to the data associated with the callback.
/// @param len
///    Size of the data pointed to by @p in, in bytes.
/// @return `0` on success, or a non-zero value to close connection.
[[gnu::access(read_only, 4, 5)]]
static int tsc_lws_cb(lws *_Nullable wsi, lws_callback_reasons reason,
                      void *_Nullable user, void *_Nullable in,
                      std::size_t len) {
  switch (reason) {
  case LWS_CALLBACK_ESTABLISHED: {
    std::array<char, sizeof("/signin")> uri;
    const int uri_len{
        lws_hdr_copy(wsi, uri.data(), uri.size(), WSI_TOKEN_GET_URI)};
    if (uri_len <= 0) {
      return 1;
    }
    if (std::string_view{uri.data(), static_cast<std::size_t>(uri_len)} !=
        "/signin") {
      return 1;
    }
    auto &session{*reinterpret_cast<ws_ctx *>(user)};
    session.s_ctx.reset(
        new signin_ctx{.cm_client = {nullptr, tek_sc_cm_client_destroy},
                       .wsi = wsi,
                       .state = signin_state::awaiting_init,
                       .type = auth_type::credentials,
                       .msg_buf = &session.tx_buf[LWS_PRE],
                       .msg_size = 0,
                       .mtx = {},
                       .account_name = {},
                       .password = {},
                       .token = {}});
    return 0;
  }
  case LWS_CALLBACK_CLOSED: {
    auto &session{*reinterpret_cast<ws_ctx *>(user)};
    std::erase(state.signin_ctxs, session.s_ctx.get());
    if (session.s_ctx->cm_client) {
      tek_sc_cm_disconnect(session.s_ctx->cm_client.get());
    } else {
      session.s_ctx->state = signin_state::disonnected;
    }
    for (auto cur_state{session.s_ctx->state};
         cur_state != signin_state::disonnected;
         cur_state = session.s_ctx->state) {
      ts3_os_futex_wait(
          reinterpret_cast<const std::atomic_uint32_t *>(&session.s_ctx->state),
          static_cast<std::uint32_t>(cur_state),
          std::numeric_limits<std::uint32_t>::max());
    }
    session.s_ctx.reset();
    return 0;
  }
  case LWS_CALLBACK_RECEIVE:
    if (lws_frame_is_binary(wsi)) {
      break;
    }
    if (lws_remaining_packet_payload(wsi) || !lws_is_final_fragment(wsi)) {
      // No incoming messages should exceed buffer size, and if they do, it's
      //    likely to be some sort of DDOS attack
      return 1;
    }
    return signin_process_msg(*reinterpret_cast<ws_ctx *>(user)->s_ctx.get(),
                              reinterpret_cast<char *>(in), len);
  case LWS_CALLBACK_SERVER_WRITEABLE: {
    auto &session{*reinterpret_cast<ws_ctx *>(user)};
    const std::scoped_lock lock{session.s_ctx->mtx};
    if (session.s_ctx->msg_size <= 0) {
      break;
    }
    if (lws_write(wsi, &session.tx_buf[LWS_PRE], session.s_ctx->msg_size,
                  LWS_WRITE_TEXT) < session.s_ctx->msg_size) {
      return 1;
    }
    return (session.s_ctx->state >= signin_state::done) ? 1 : 0;
  }
  case LWS_CALLBACK_HTTP: {
    char *uri;
    int uri_len;
    const int method{lws_http_get_uri_and_method(wsi, &uri, &uri_len)};
    if (method < 0) {
      return 1;
    }
    const std::string_view uri_view{uri, static_cast<std::size_t>(uri_len)};
    auto &session{*reinterpret_cast<http_ctx *>(user)};
    auto buf_cur{session.tx_buf.begin()};
    const auto buf_end{session.tx_buf.end()};
    bool send_status_body{true};
    auto status{HTTP_STATUS_NOT_FOUND};
    if (state.cur_status.load(std::memory_order::relaxed) != status::running) {
      status = HTTP_STATUS_SERVICE_UNAVAILABLE;
      goto send_status;
    }
    if (uri_view == "/manifest" || uri_view == "/manifest-bin") {
      if (method != LWSHUMETH_GET) {
        status = HTTP_STATUS_METHOD_NOT_ALLOWED;
        goto send_status;
      }
      std::array<char, 256> hdr_buf;
      // Check If-Modified-Since header
      int hdr_len{lws_hdr_copy(wsi, hdr_buf.data(), hdr_buf.size(),
                               WSI_TOKEN_HTTP_IF_MODIFIED_SINCE)};
      if (hdr_len < 0) {
        return 1;
      }
      const std::scoped_lock lock{state.manifest_mtx};
      if (hdr_len) {
        std::istringstream stream{
            {hdr_buf.data(), static_cast<std::size_t>(hdr_len)}};
        stream.imbue(std::locale::classic());
        std::tm tm;
        stream >> std::get_time(&tm, "%a, %d %b %Y %X GMT");
        if (!stream.fail()) {
          tm.tm_isdst = 0;
          if (state.timestamp <= timegm(&tm)) {
            send_status_body = false;
            status = HTTP_STATUS_NOT_MODIFIED;
            goto send_status;
          }
        }
      }
      // Read Accept-Encoding header
      hdr_len = lws_hdr_copy(wsi, hdr_buf.data(), hdr_buf.size(),
                             WSI_TOKEN_HTTP_ACCEPT_ENCODING);
      if (hdr_len < 0) {
        return 1;
      }
      // Select response encoding
      const bool binary{uri_view != "/manifest"};
      const auto &buf{binary ? state.manifest_bin : state.manifest};
      const auto enc{negotiate_enc(
          {hdr_buf.data(), static_cast<std::size_t>(hdr_len)}, buf)};
      auto set_enc{[&hdr_buf, &hdr_len](const std::string_view &&name) {
        std::ranges::copy(name, hdr_buf.data());
        hdr_len = name.length();
      }};
      switch (enc) {
      case enc_type::none:
        session.data = {buf.buf.buf.get(), buf.buf.size};
        break;
      case enc_type::deflate: {
        session.data = {buf.deflate.buf.get(), buf.deflate.size};
        set_enc("deflate");
        break;
      }
#ifdef TEK_S3B_BROTLI
      case enc_type::brotli: {
        session.data = {buf.brotli.buf.get(), buf.brotli.size};
        set_enc("br");
        break;
      }
#endif // def TEK_S3B_BROTLI
#ifdef TEK_S3B_ZSTD
      case enc_type::zstd: {
        session.data = {buf.zstd.buf.get(), buf.zstd.size};
        set_enc("zstd");
        break;
      }
#endif // def TEK_S3B_ZSTD
      }
      // Write headers
      if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK,
                                      binary
                                          ? "application/octet-stream"
                                          : "application/json; charset=utf-8",
                                      session.data.size(), &buf_cur, buf_end)) {
        return 1;
      }
      if (constexpr std::string_view cache_control{"no-cache"};
          lws_add_http_header_by_token(
              wsi, WSI_TOKEN_HTTP_CACHE_CONTROL,
              reinterpret_cast<const unsigned char *>(cache_control.data()),
              cache_control.length(), &buf_cur, buf_end)) {
        return 1;
      }
      if (enc != enc_type::none) {
        if (lws_add_http_header_by_token(
                wsi, WSI_TOKEN_HTTP_CONTENT_ENCODING,
                reinterpret_cast<const unsigned char *>(hdr_buf.data()),
                hdr_len, &buf_cur, buf_end)) {
          return 1;
        }
      }
      // Set Last-Modified header
      const auto res{std::format_to_n(
          hdr_buf.data(), hdr_buf.size(), std::locale::classic(),
          "{:%a, %d %b %Y %X} GMT",
          std::chrono::system_clock::from_time_t(state.timestamp))};
      if (const std::string_view last_mod{hdr_buf.data(), res.out};
          lws_add_http_header_by_token(
              wsi, WSI_TOKEN_HTTP_LAST_MODIFIED,
              reinterpret_cast<const unsigned char *>(last_mod.data()),
              last_mod.length(), &buf_cur, buf_end)) {
        return 1;
      }
      if (lws_finalize_http_header(wsi, &buf_cur, buf_end)) {
        return 1;
      }
      // Determine response body size
      const auto send_size{std::min<std::size_t>(
          session.data.size(), std::distance(buf_cur, buf_end))};
      const bool done{send_size == session.data.size()};
      buf_cur =
          std::ranges::copy(session.data.subspan(0, send_size), buf_cur).out;
      // Send the response packet
      if (const int size{
              static_cast<int>(std::distance(session.tx_buf.begin(), buf_cur))};
          lws_write(wsi, session.tx_buf.data(), size,
                    done ? LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP) < size) {
        return 1;
      }
      if (done) {
        // Close connection
        return 1;
      }
      // More data to come
      session.data = session.data.subspan(send_size);
      state.download_lock.lock();
      lws_callback_on_writable(wsi);
      return 0;
    } else if (uri_view == "/mrc") { // if (uri_view == "/manifest")
      if (method != LWSHUMETH_GET) {
        status = HTTP_STATUS_METHOD_NOT_ALLOWED;
        goto send_status;
      }
      // Parse URL arguments
      std::array<char, sizeof("manifest_id=") + 21> buf;
      int buf_len{
          lws_get_urlarg_by_name_safe(wsi, "app_id=", buf.data(), buf.size())};
      if (buf_len <= 0) {
        status = HTTP_STATUS_BAD_REQUEST;
        goto send_status;
      }
      std::uint32_t app_id;
      if (std::from_chars(buf.data(), &buf[buf_len], app_id).ec !=
          std::errc{}) {
        status = HTTP_STATUS_BAD_REQUEST;
        goto send_status;
      }
      buf_len =
          lws_get_urlarg_by_name_safe(wsi, "depot_id=", buf.data(), buf.size());
      if (buf_len <= 0) {
        status = HTTP_STATUS_BAD_REQUEST;
        goto send_status;
      }
      std::uint32_t depot_id;
      if (std::from_chars(buf.data(), &buf[buf_len], depot_id).ec !=
          std::errc{}) {
        status = HTTP_STATUS_BAD_REQUEST;
        goto send_status;
      }
      buf_len = lws_get_urlarg_by_name_safe(wsi, "manifest_id=", buf.data(),
                                            buf.size());
      if (buf_len <= 0) {
        status = HTTP_STATUS_BAD_REQUEST;
        goto send_status;
      }
      std::uint64_t manifest_id;
      if (std::from_chars(buf.data(), &buf[buf_len], manifest_id).ec !=
          std::errc{}) {
        status = HTTP_STATUS_BAD_REQUEST;
        goto send_status;
      }
      std::uint64_t mrc;
      int rem_time;
      // Check if the manifest request code is present in the cache
      const auto it{state.mrcs.find(manifest_id)};
      mrc = it == state.mrcs.end() ? 0 : it->second.mrc;
      if (!mrc) {
        // If not, fetch it from Steam CM
        std::unique_lock lock{state.manifest_mtx};
        const auto &app{state.apps.find(app_id)};
        if (app == state.apps.end()) {
          status = HTTP_STATUS_UNAUTHORIZED;
          goto send_status;
        }
        const auto depot{app->second.depots.find(depot_id)};
        if (depot == app->second.depots.end()) {
          status = HTTP_STATUS_UNAUTHORIZED;
          goto send_status;
        }
        const auto cm_client{(*depot->second.next_acc)->cm_client};
        if (++depot->second.next_acc == depot->second.accs.cend()) {
          depot->second.next_acc = depot->second.accs.cbegin();
        }
        lock.unlock();
        mrc_await_entry entry{
            .data = {.app_id = app_id,
                     .depot_id = depot_id,
                     .manifest_id = manifest_id,
                     .request_code = 0,
                     .result = {.type = TEK_SC_ERR_TYPE_basic,
                                .primary = TEK_SC_ERRC_cm_timeout,
                                .auxiliary = 0,
                                .extra = 0,
                                .uri = nullptr}},
            .finished = 0};
        tek_sc_cm_get_mrc(cm_client, &entry.data, cb_mrc, 2000);
        ts3_os_futex_wait(&entry.finished, 0, 2000);
        if (!tek_sc_err_success(&entry.data.result)) {
          if (entry.data.result.type == TEK_SC_ERR_TYPE_sub &&
              entry.data.result.auxiliary == TEK_SC_ERRC_cm_timeout) {
            status = HTTP_STATUS_GATEWAY_TIMEOUT;
          } else {
            status = HTTP_STATUS_INTERNAL_SERVER_ERROR;
          }
          goto send_status;
        }
        mrc = entry.data.request_code;
        if (state.mrcs.size() >= 128) {
          // Don't keep more than 128 entries in the cache at any given time, to
          //    avoid memory overflows
          const auto first_it{state.mrcs.begin()};
          lws_sul_cancel(&first_it->second.sul);
          state.mrcs.erase(first_it);
        }
        auto &cache_entry{
            state.mrcs.emplace(manifest_id, mrc_cache{}).first->second};
        // Steam refreshes MRCs on every *4 and *9 minute, that is every 5
        //    minutes with offset of 240 seconds from 5-minute boundary, use
        //    that info to schedule cache entry removal on next refresh
        const auto now{std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now())};
        rem_time = ((now + 60) / 300 * 300 + 240) - now;
        cache_entry.sul.us = lws_now_usecs() + rem_time * LWS_US_PER_SEC;
        cache_entry.sul.cb = remove_mrc_cache;
        cache_entry.manifest_id = manifest_id;
        cache_entry.mrc = mrc;
        lws_sul2_schedule(state.lws_ctx, 0, LWSSULLI_MISS_IF_SUSPENDED,
                          &cache_entry.sul);
      } else { // if (it == state.mrcs.end())
        mrc = it->second.mrc;
        rem_time = (it->second.sul.us - lws_now_usecs()) / LWS_US_PER_SEC;
      } // if (it == state.mrcs.end()) else
      const auto res{std::to_chars(buf.begin(), buf.end(), mrc)};
      if (res.ec != std::errc{}) {
        status = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        goto send_status;
      }
      const std::string_view mrc_view{buf.data(), res.ptr};
      // Write headers
      if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK,
                                      "text/plain; charset=utf-8",
                                      mrc_view.length(), &buf_cur, buf_end)) {
        return 1;
      }
      std::array<char, sizeof("max-age=") + 3> cache_control_buf;
      if (const std::string_view cache_control{
              cache_control_buf.data(),
              std::format_to_n(cache_control_buf.data(),
                               cache_control_buf.size(), std::locale::classic(),
                               "max-age={}", rem_time)
                  .out};
          lws_add_http_header_by_token(
              wsi, WSI_TOKEN_HTTP_CACHE_CONTROL,
              reinterpret_cast<const unsigned char *>(cache_control.data()),
              cache_control.length(), &buf_cur, buf_end)) {
        return 1;
      }
      if (lws_finalize_http_header(wsi, &buf_cur, buf_end)) {
        return 1;
      }
      // Send the response
      buf_cur = std::ranges::copy(mrc_view, buf_cur).out;
      lws_write(wsi, session.tx_buf.data(),
                std::distance(session.tx_buf.data(), buf_cur),
                LWS_WRITE_HTTP_FINAL);
      return 1;
    } // if (uri_view == "/manifest") else if (uri_view == "/mrc")
  send_status:
    // Send just the status code and (unless disabled) its text representation
    //    as the body
    if (send_status_body) {
      std::array<char, 10> status_buf;
      const auto res{std::to_chars(status_buf.begin(), status_buf.end(),
                                   static_cast<unsigned>(status))};
      if (res.ec != std::errc{}) {
        return 1;
      }
      const std::string_view status_view{status_buf.data(), res.ptr};
      if (lws_add_http_common_headers(wsi, status, "text/plain; charset=utf-8",
                                      status_view.length(), &buf_cur,
                                      buf_end)) {
        return 1;
      }
      if (lws_finalize_http_header(wsi, &buf_cur, buf_end)) {
        return 1;
      }
      buf_cur = std::ranges::copy(status_view, buf_cur).out;
    } else {
      if (lws_add_http_common_headers(wsi, status, nullptr, 0, &buf_cur,
                                      buf_end)) {
        return 1;
      }
      if (lws_finalize_http_header(wsi, &buf_cur, buf_end)) {
        return 1;
      }
    }
    lws_write(wsi, session.tx_buf.data(),
              std::distance(session.tx_buf.begin(), buf_cur),
              LWS_WRITE_HTTP_FINAL);
    return 1;
  } // case LWS_CALLBACK_HTTP
  case LWS_CALLBACK_HTTP_WRITEABLE: {
    if (!user) {
      // Ignore unrelated callbacks
      break;
    }
    auto &session{*reinterpret_cast<http_ctx *>(user)};
    const int send_size{
        static_cast<int>(std::min(session.data.size(), tx_size))};
    const bool done{send_size == static_cast<int>(session.data.size())};
    if (lws_write(wsi, session.data.data(), send_size,
                  done ? LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP) < send_size) {
      state.download_lock.unlock();
      return 1;
    }
    if (done) {
      // Close connection
      state.download_lock.unlock();
      return 1;
    }
    // More data to come
    session.data = session.data.subspan(send_size);
    lws_callback_on_writable(wsi);
    return 0;
  }
  case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
    std::unique_lock lock{state.manifest_mtx};
    if (state.cur_status.load(std::memory_order::relaxed) == status::stopping) {
      state.download_lock.force_unlock();
      // Destroy libwebsockets context
      const auto lws_ctx{state.lws_ctx};
      state.lws_ctx = nullptr;
      for (auto &acc : state.accounts | std::views::values) {
        if (acc.ren_status == renew_status::scheduled) {
          lws_sul_cancel(&acc.sul);
        }
      }
      lws_context_destroy(lws_ctx);
      return 1;
    }
    for (auto &acc : state.accounts | std::views::values) {
      if (acc.rem_status.load(std::memory_order::relaxed) ==
              remove_status::remove &&
          acc.ren_status == renew_status::scheduled) {
        lws_sul_cancel(&acc.sul);
      } else if (acc.ren_status == renew_status::pending_schedule) {
        lws_sul2_schedule(state.lws_ctx, 0, LWSSULLI_MISS_IF_SUSPENDED,
                          &acc.sul);
        acc.ren_status = renew_status::scheduled;
      }
    }
    if (std::erase_if(state.accounts,
                      [](const auto &pair) {
                        return pair.second.rem_status.load(
                                   std::memory_order::relaxed) ==
                               remove_status::remove;
                      }) &&
        state.cur_status.load(std::memory_order::relaxed) == status::setup &&
        state.num_ready_accs == static_cast<int>(state.accounts.size())) {
      update_manifest();
      state.cur_status.store(status::running, std::memory_order::relaxed);
    }
    lock.unlock();
    for (auto ctx : state.signin_ctxs) {
      if (ctx->msg_size > 0) {
        lws_callback_on_writable(ctx->wsi);
      } else if (ctx->msg_size < 0) {
        lws_set_timeout(ctx->wsi, static_cast<pending_timeout>(1),
                        LWS_TO_KILL_ASYNC);
      }
    }
    break;
  } // case LWS_CALLBACK_EVENT_WAIT_CANCELLED
  default:
    break;
  } // switch (reason)
  return lws_callback_http_dummy(wsi, reason, user, in, len);
}

} // namespace

//===-- Internal variable -------------------------------------------------===//

constexpr lws_protocols protocol{.name = "tek-s3",
                                 .callback = tsc_lws_cb,
                                 .per_session_data_size =
                                     std::max(sizeof(http_ctx), sizeof(ws_ctx)),
                                 .rx_buffer_size = 32768,
                                 .id = 0,
                                 .user = nullptr,
                                 .tx_packet_size = tx_size};

} // namespace tek::s3

//===-- Internal function -------------------------------------------------===//

extern "C" void ts3_run(void) {
  while (!lws_service(tek::s3::state.lws_ctx, 0))
    ;
}
