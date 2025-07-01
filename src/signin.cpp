//===-- signin.cpp - Steam account sign-in handling implementation --------===//
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
/// Implementation of @ref tek::s3::signin_process_msg and related CM client
///    callback functions.
///
//===----------------------------------------------------------------------===//
#include "signin.hpp"

#include "cm_callbacks.hpp"
#include "config.h"
#include "null_attrs.h" // IWYU pragma: keep
#include "os.h"
#include "state.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <libwebsockets.h>
#include <memory>
#include <mutex>
#include <rapidjson/document.h>
#include <rapidjson/reader.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <set>
#include <span>
#include <string_view>
#include <tek-steamclient/cm.h>
#include <tek-steamclient/error.h>
#include <utility>

namespace tek::s3 {

namespace {

//===-- Private functions -------------------------------------------------===//

/// The callback for CM client authentication session events.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in] data
///    Pointer to `tek_sc_cm_data_auth_polling` describing the event.
/// @param [in, out] user_data
///    Pointer to the @ref signin_ctx object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_only, 2),
  access(read_write, 3)]]
static void cb_auth(tek_sc_cm_client *_Nonnull client, void *_Nonnull data,
                    void *_Nonnull user_data) {
  const auto &data_auth =
      *reinterpret_cast<const tek_sc_cm_data_auth_polling *>(data);
  auto &ctx = *reinterpret_cast<signin_ctx *>(user_data);
  const std::scoped_lock lock(ctx.mtx);
  switch (data_auth.status) {
  case TEK_SC_CM_AUTH_STATUS_completed: {
    rapidjson::StringBuffer buf;
    rapidjson::Writer writer(buf);
    writer.StartObject();
    if (tek_sc_err_success(&data_auth.result)) {
      ctx.token = data_auth.token;
      ctx.state = signin_state::done;
      const auto token_info = tek_sc_cm_parse_auth_token(data_auth.token);
      std::string_view str = "renewable";
      writer.Key(str.data(), str.length());
      writer.Bool(token_info.renewable);
      if (!token_info.renewable) {
        str = "expires";
        writer.Key(str.data(), str.length());
        writer.Uint64(static_cast<std::uint64_t>(token_info.expires));
      }
    } else { // if (tek_sc_err_success(&data_auth.result))
      std::string_view str = "error";
      writer.Key(str.data(), str.length());
      writer.StartObject();
      str = "type";
      writer.Key(str.data(), str.length());
      writer.Int(static_cast<int>(data_auth.result.type));
      str = "primary";
      writer.Key(str.data(), str.length());
      writer.Int(static_cast<int>(data_auth.result.primary));
      if (data_auth.result.type != TEK_SC_ERR_TYPE_basic) {
        str = "auxiliary";
        writer.Key(str.data(), str.length());
        writer.Int(data_auth.result.auxiliary);
      }
      writer.EndObject();
    } // if (tek_sc_err_success(&data_auth.result)) else
    writer.EndObject();
    ctx.msg_size = buf.GetSize();
    if (LWS_PRE + static_cast<std::size_t>(ctx.msg_size) > tx_size) {
      ctx.msg_size = -1;
    } else {
      std::ranges::copy_n(buf.GetString(), ctx.msg_size, ctx.msg_buf);
    }
    lws_cancel_service(state.lws_ctx);
    tek_sc_cm_disconnect(client);
    break;
  } // case TEK_SC_CM_AUTH_STATUS_completed
  case TEK_SC_CM_AUTH_STATUS_new_url: {
    rapidjson::StringBuffer buf;
    rapidjson::Writer writer(buf);
    writer.StartObject();
    std::string_view str = "url";
    writer.Key(str.data(), str.length());
    str = data_auth.url;
    writer.String(str.data(), str.length());
    writer.EndObject();
    ctx.msg_size = buf.GetSize();
    if (LWS_PRE + static_cast<std::size_t>(ctx.msg_size) > tx_size) {
      ctx.msg_size = -1;
    } else {
      std::ranges::copy_n(buf.GetString(), ctx.msg_size, ctx.msg_buf);
    }
    lws_cancel_service(state.lws_ctx);
    break;
  }
  case TEK_SC_CM_AUTH_STATUS_awaiting_confirmation: {
    rapidjson::StringBuffer buf;
    rapidjson::Writer writer(buf);
    writer.StartObject();
    std::string_view str = "confirmations";
    writer.Key(str.data(), str.length());
    writer.StartArray();
    if (data_auth.confirmation_types &
        TEK_SC_CM_AUTH_CONFIRMATION_TYPE_device) {
      str = "device";
      writer.String(str.data(), str.length());
    }
    if (data_auth.confirmation_types &
        TEK_SC_CM_AUTH_CONFIRMATION_TYPE_guard_code) {
      str = "guard_code";
      writer.String(str.data(), str.length());
    }
    if (data_auth.confirmation_types & TEK_SC_CM_AUTH_CONFIRMATION_TYPE_email) {
      str = "email";
      writer.String(str.data(), str.length());
    }
    writer.EndArray();
    writer.EndObject();
    ctx.state = signin_state::awaiting_confirmation;
    ctx.msg_size = buf.GetSize();
    if (LWS_PRE + static_cast<std::size_t>(ctx.msg_size) > tx_size) {
      ctx.msg_size = -1;
    } else {
      std::ranges::copy_n(buf.GetString(), ctx.msg_size, ctx.msg_buf);
    }
    lws_cancel_service(state.lws_ctx);
  }
  } // switch (data_auth.status)
}

/// The callback for CM client connected event.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in] data
///    Pointer to `tek_sc_err` indicating the result of connection.
/// @param [in, out] user_data
///    Pointer to the @ref signin_ctx object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_only, 2),
  access(read_write, 3)]]
static void cb_auth_connected(tek_sc_cm_client *_Nonnull client,
                              void *_Nonnull data, void *_Nonnull user_data) {
  auto &ctx = *reinterpret_cast<signin_ctx *>(user_data);
  if (const auto &res = *reinterpret_cast<const tek_sc_err *>(data);
      !tek_sc_err_success(&res)) {
    const std::scoped_lock lock(ctx.mtx);
    if (ctx.state != signin_state::done) {
      ctx.state = signin_state::disonnected;
      rapidjson::StringBuffer buf;
      rapidjson::Writer writer(buf);
      writer.StartObject();
      std::string_view str = "error";
      writer.Key(str.data(), str.length());
      writer.StartObject();
      str = "type";
      writer.Key(str.data(), str.length());
      writer.Int(static_cast<int>(res.type));
      str = "primary";
      writer.Key(str.data(), str.length());
      writer.Int(static_cast<int>(res.primary));
      if (res.type != TEK_SC_ERR_TYPE_basic) {
        str = "auxiliary";
        writer.Key(str.data(), str.length());
        writer.Int(res.auxiliary);
      }
      writer.EndObject();
      writer.EndObject();
      ctx.msg_size = buf.GetSize();
      if (LWS_PRE + static_cast<std::size_t>(ctx.msg_size) > tx_size) {
        ctx.msg_size = -1;
      } else {
        std::ranges::copy_n(buf.GetString(), ctx.msg_size, ctx.msg_buf);
      }
      lws_cancel_service(state.lws_ctx);
    }
    return;
  } // if (!tek_sc_err_success(&res))
  constexpr std::string_view dev_name_pfx = "tek-s3 " TEK_S3_VERSION " @ ";
  static std::array<char, dev_name_pfx.length() + 32> dev_name =
      [&dev_name_pfx]() {
        std::array<char, dev_name.size()> buf;
        std::ranges::copy(dev_name_pfx, buf.data());
        const std::span<char, buf.size() - dev_name_pfx.length()> hostname(
            &buf[dev_name_pfx.length()], hostname.extent);
        ts3_os_get_hostname(hostname.data(), hostname.size());
        return buf;
      }();
  const std::scoped_lock lock(ctx.mtx);
  switch (ctx.type) {
  case auth_type::credentials:
    tek_sc_cm_auth_credentials(client, dev_name.data(), ctx.account_name.data(),
                               ctx.password.data(), cb_auth, 3000);
    break;
  case auth_type::qr:
    tek_sc_cm_auth_qr(client, dev_name.data(), cb_auth, 3000);
  }
}

/// The callback for CM client disconnected event.
///
/// @param [in, out] user_data
///    Pointer to the associated @ref signin_ctx object.
[[using gnu: nonnull(3), access(read_write, 3)]]
static void cb_auth_disconnected(tek_sc_cm_client *, void *,
                                 void *_Nonnull user_data) {
  auto &ctx = *reinterpret_cast<signin_ctx *>(user_data);
  const std::scoped_lock lock(ctx.mtx);
  if (ctx.state == signin_state::done) {
    if (!ctx.token.empty()) {
      state.manifest_mtx.lock();
      auto cm_client = ctx.cm_client.release();
      const auto token_info = tek_sc_cm_parse_auth_token(ctx.token.data());
      const auto [it, emplaced] = state.accounts.try_emplace(
          token_info.steam_id, lws_sorted_usec_list_t{}, cm_client,
          std::move(ctx.token), token_info, renew_status::not_scheduled, 0, 0,
          remove_status::none, std::unique_ptr<tek_sc_cm_data_depot_key[]>{},
          std::set<std::uint32_t>{});
      auto &acc = it->second;
      if (emplaced) {
        // New account added
        tek_sc_cm_set_user_data(cm_client, &acc);
        state.state_dirty = true;
        update_manifest();
        state.manifest_mtx.unlock();
        tek_sc_cm_connect(cm_client, cb_connected, 5000, cb_disconnected);
      } else if (token_info.renewable && !acc.token_info.renewable) {
        // Account already present, but new token for it is renewable unlike the
        // current one, so replace it
        tek_sc_cm_set_user_data(cm_client, &acc);
        acc.token = std::move(ctx.token);
        acc.token_info = token_info;
        std::swap(cm_client, acc.cm_client);
        state.state_dirty = true;
        update_manifest();
        state.manifest_mtx.unlock();
        tek_sc_cm_connect(acc.cm_client, cb_connected, 5000, cb_disconnected);
        tek_sc_cm_client_destroy(cm_client);
      } else {
        // Account already present, discard new token
        state.manifest_mtx.unlock();
        tek_sc_cm_client_destroy(cm_client);
      }
    }
  } else if (state.cur_status.load(std::memory_order::relaxed) !=
             status::stopping) {
    ctx.msg_size = -1;
    lws_cancel_service(state.lws_ctx);
  }
  ctx.state = signin_state::disonnected;
  ts3_os_futex_wake(reinterpret_cast<std::atomic_uint32_t *>(&ctx.state));
}

} // namespace

//===-- Internal function -------------------------------------------------===//

int signin_process_msg(signin_ctx &ctx, char *msg, std::size_t msg_size) {
  const std::scoped_lock lock(ctx.mtx);
  if (ctx.state == signin_state::awaiting_cm_response) {
    return 1;
  }
  // Null-terminate the buffer for in-situ parsing to work correctly
  msg[msg_size] = '\0';
  rapidjson::Document doc;
  doc.ParseInsitu<rapidjson::kParseStopWhenDoneFlag>(msg);
  if (doc.HasParseError() || !doc.IsObject()) {
    return 1;
  }
  switch (ctx.state) {
  case signin_state::awaiting_init: {
    const auto type = doc.FindMember("type");
    if (type == doc.MemberEnd() || !type->value.IsString()) {
      return 1;
    }
    const std::string_view type_view(type->value.GetString(),
                                     type->value.GetStringLength());
    if (type_view == "credentials") {
      ctx.type = auth_type::credentials;
      const auto account_name = doc.FindMember("account_name");
      if (account_name == doc.MemberEnd() || !account_name->value.IsString()) {
        return 1;
      }
      ctx.account_name = {account_name->value.GetString(),
                          account_name->value.GetStringLength()};
      const auto password = doc.FindMember("password");
      if (password == doc.MemberEnd() || !password->value.IsString()) {
        return 1;
      }
      ctx.password = {password->value.GetString(),
                      password->value.GetStringLength()};
    } else if (type_view == "qr") {
      ctx.type = auth_type::qr;
    } else {
      return 1;
    }
    ctx.cm_client.reset(tek_sc_cm_client_create(state.tek_sc_ctx, &ctx));
    if (!ctx.cm_client) {
      return 1;
    }
    state.signin_ctxs.emplace_back(&ctx);
    ctx.state = signin_state::awaiting_cm_response;
    tek_sc_cm_connect(ctx.cm_client.get(), cb_auth_connected, 5000,
                      cb_auth_disconnected);
    return 0;
  }
  case signin_state::awaiting_confirmation: {
    const auto type = doc.FindMember("type");
    if (type == doc.MemberEnd() || !type->value.IsString()) {
      return 1;
    }
    const std::string_view type_view(type->value.GetString(),
                                     type->value.GetStringLength());
    tek_sc_cm_auth_confirmation_type conf_type;
    if (type_view == "guard_code") {
      conf_type = TEK_SC_CM_AUTH_CONFIRMATION_TYPE_guard_code;
    } else if (type_view == "email") {
      conf_type = TEK_SC_CM_AUTH_CONFIRMATION_TYPE_email;
    } else {
      return 1;
    }
    const auto code = doc.FindMember("code");
    if (code == doc.MemberEnd() || !code->value.IsString()) {
      return 1;
    }
    tek_sc_cm_auth_submit_code(ctx.cm_client.get(), conf_type,
                               code->value.GetString());
    return 0;
  }
  default:
    return 1;
  } // switch (ctx.state)
}

} // namespace tek::s3
