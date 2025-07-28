//===-- cm_callbacks.cpp - CM client callbacks implementation -------------===//
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
/// Implementation of Steam CM callback functions.
///
//===----------------------------------------------------------------------===//
#include "cm_callbacks.hpp"

#include "null_attrs.h" // IWYU pragma: keep
#include "os.h"
#include "state.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <libwebsockets.h>
#include <memory>
#include <mutex>
#include <print>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <system_error>
#include <tek-steamclient/cm.h>
#include <tek-steamclient/error.h>
#include <unordered_map>
#include <utility>
#include <vdf_parser.hpp>
#include <vector>

namespace tek::s3 {

namespace {

//===-- Private type ------------------------------------------------------===//

/// Binary VDF node structure.
struct [[gnu::visibility("internal")]] bin_vdf_node {
  /// Integer attributes of the node.
  std::unordered_map<std::string_view, std::int32_t> int_attrs;
  /// String attributes of the node.
  std::unordered_map<std::string_view, std::string_view> str_attrs;
  /// Child nodes.
  std::unordered_map<std::string_view, std::shared_ptr<bin_vdf_node>> children;

  /// Parse a binary VDF node.
  ///
  /// @param [in, out] cur
  ///    Reference to pointer to the current position in the buffer.
  /// @param [in] end
  ///    Pointer to the end of the buffer.
  bin_vdf_node(const char *_Nullable &cur, const char *_Nullable end) {
    while (cur < end) {
      const char byte = *cur++;
      if (byte == '\x08') {
        return;
      }
      auto nullt = std::ranges::find(cur, end, '\0');
      if (nullt == end) {
        return;
      }
      const std::string_view name(cur, nullt);
      cur = nullt + 1;
      switch (byte) {
      case '\x00': {
        children.emplace(name, std::make_shared<bin_vdf_node>(cur, end));
        break;
      }
      case '\x01':
        nullt = std::ranges::find(cur, end, '\0');
        if (nullt == end) {
          return;
        }
        str_attrs.emplace(name, std::string_view{cur, nullt});
        cur = nullt + 1;
        break;
      case '\x02':
        std::int32_t val;
        if (std::distance(cur, end) < static_cast<std::ptrdiff_t>(sizeof val)) {
          return;
        }
        std::memcpy(&val, cur, sizeof val);
        int_attrs.emplace(name, val);
        cur += sizeof val;
        break;
      default:
        return;
      }
    }
  }
};

//===-- Private functions -------------------------------------------------===//

/// Print a tek-steamclient error message to stderr.
///
/// @param [in] err
///    tek-steamclient error description.
static void print_err(const tek_sc_err &err) noexcept {
  auto msgs = tek_sc_err_get_msgs(&err);
  std::println(std::cerr, "  Error type: ({}) {}\n  Primary message: ({}) {}",
               static_cast<int>(err.type), msgs.type_str,
               static_cast<int>(err.primary), msgs.primary);
  if (err.type != TEK_SC_ERR_TYPE_basic) {
    std::println(std::cerr, "  Auxiliary message: ({}) {}", err.auxiliary,
                 msgs.auxiliary);
    if (msgs.extra) {
      std::println(std::cerr, "{}", msgs.extra);
    }
  }
  if (err.uri) {
    std::println(std::cerr, "  {}: {}", msgs.uri_type, err.uri);
    std::free(const_cast<char *>(err.uri));
  }
  tek_sc_err_release_msgs(&msgs);
}

static void sync_manifest() {
  const std::scoped_lock lock(state.manifest_mtx);
  for (auto &app : state.apps | std::views::values) {
    if (std::erase_if(app.depots, [](const auto &pair) {
          return pair.second.accs.empty();
        })) {
      state.manifest_dirty = true;
    }
  }
  if (std::erase_if(state.apps, [](const auto &app) {
        return app.second.depots.empty();
      })) {
    state.manifest_dirty = true;
  }
  update_manifest();
}

/// The callback for CM client depot decryption key received event.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in, out] data
///    Pointer to `tek_sc_cm_data_depot_key` associated with request.
/// @param [in, out] user_data
///    Pointer to the @ref account object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_write, 2),
  access(read_write, 3)]]
static void cb_depot_key(tek_sc_cm_client *_Nonnull client, void *_Nonnull data,
                         void *_Nonnull user_data) {
  if (state.cur_status.load(std::memory_order::relaxed) == status::stopping) {
    return;
  }
  auto &data_dk = *reinterpret_cast<tek_sc_cm_data_depot_key *>(data);
  auto &acc = *reinterpret_cast<account *>(user_data);
  // TEK_SC_CM_ERESULT_blocked is returned for pre-download depots, ignore it
  if (!tek_sc_err_success(&data_dk.result) &&
      data_dk.result.type != TEK_SC_ERR_TYPE_steam_cm &&
      data_dk.result.auxiliary != TEK_SC_CM_ERESULT_blocked) {
    if (data_dk.result.type == TEK_SC_ERR_TYPE_sub &&
        data_dk.result.auxiliary == TEK_SC_ERRC_cm_timeout) {
      // Timeouts are common for depot key requests, just re-send it
      tek_sc_cm_get_depot_key(client, &data_dk, cb_depot_key, 3000);
      return;
    }
    std::println(std::cerr, "Failed to get decryption key for depot {}:",
                 data_dk.depot_id);
    print_err(data_dk.result);
    tek_sc_cm_disconnect(client);
    return;
  }
  if (tek_sc_err_success(&data_dk.result)) {
    const std::scoped_lock lock(state.manifest_mtx);
    state.manifest_dirty = true;
    std::ranges::move(data_dk.key, state.depot_keys[data_dk.depot_id]);
  }
  if (!--acc.rem_dk_burst) {
    if (acc.rem_dk_total) {
      // Schedule the next burst
      const int burst_size = std::min(5, acc.rem_dk_total);
      acc.rem_dk_burst = burst_size;
      for (int i = 0; i < burst_size; ++i) {
        tek_sc_cm_get_depot_key(client,
                                &acc.depot_key_requests[--acc.rem_dk_total],
                                cb_depot_key, 3000);
      }
    } else {
      acc.depot_key_requests.reset();
      if (state.cur_status.load(std::memory_order::relaxed) ==
          status::running) {
        sync_manifest();
      }
    }
  }
}

/// The callback for CM client PICS app info received event.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in, out] data
///    Pointer to `tek_sc_cm_data_pics` associated with the request.
/// @param [in, out] user_data
///    Pointer to the @ref account object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_write, 2),
  access(read_write, 3)]]
static void cb_app_info(tek_sc_cm_client *_Nonnull client, void *_Nonnull data,
                        void *_Nonnull user_data) {
  auto &data_pics = *reinterpret_cast<tek_sc_cm_data_pics *>(data);
  auto &acc = *reinterpret_cast<account *>(user_data);
  if (!tek_sc_err_success(&data_pics.result)) {
    std::println(std::cerr, "Failed to get PICS info for account {}'s apps:",
                 acc.token_info.steam_id);
    print_err(data_pics.result);
    delete[] data_pics.app_entries;
    delete &data_pics;
    tek_sc_cm_disconnect(client);
    return;
  }
  // App/depot IDs which don't have their decryption keys cached yet.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> missing_keys;
  const std::span apps(data_pics.app_entries, data_pics.num_app_entries);
  for (const auto &app : apps) {
    if (tek_sc_err_success(&app.result)) {
      continue;
    }
    // Some apps are just weird and don't provide an access token.
    if (app.result.type == TEK_SC_ERR_TYPE_sub &&
        app.result.auxiliary == TEK_SC_ERRC_cm_missing_token) {
      continue;
    }
    std::println(std::cerr,
                 "Could not get PICS info for app {} owned by account {}:",
                 app.id, acc.token_info.steam_id);
    print_err(app.result);
    goto app_err;
  }
  // Extract depot IDs and name from the app info
  for (const std::scoped_lock lock(state.manifest_mtx);
       const auto &app : apps) {
    if (!tek_sc_err_success(&app.result)) {
      continue;
    }
    std::string_view view(reinterpret_cast<const char *>(app.data),
                          app.data_size);
    std::error_code ec;
    const auto vdf = tyti::vdf::read(view.begin(), view.end(), ec);
    if (ec != std::error_code{}) {
      std::println(
          std::cerr,
          "Failed to parse VDF app info for app {} owned by account {}:",
          app.id, acc.token_info.steam_id);
      goto app_err;
    }
    const auto depots = vdf.childs.find("depots");
    if (depots != vdf.childs.cend()) {
      // Collect IDs of depots present in the app
      std::vector<std::uint32_t> depot_ids;
      if (const auto workshopdepot =
              depots->second->attribs.find("workshopdepot");
          workshopdepot != depots->second->attribs.cend()) {
        view = workshopdepot->second;
        if (std::uint32_t depot_id;
            std::from_chars(view.begin(), view.end(), depot_id).ec ==
            std::errc{}) {
          depot_ids.emplace_back(depot_id);
        }
      }
      for (const auto &[id, depot] : depots->second->childs) {
        if (!depot->childs.contains("manifests")) {
          continue;
        }
        view = id;
        std::uint32_t depot_id;
        if (std::from_chars(view.begin(), view.end(), depot_id).ec !=
            std::errc{}) {
          continue;
        }
        const auto it = acc.depot_ids.find(depot_id);
        if (it != acc.depot_ids.end()) {
          acc.depot_ids.erase(it);
          depot_ids.emplace_back(depot_id);
        }
      }
      if (!depot_ids.empty()) {
        const auto [it, emplaced] = state.apps.try_emplace(app.id);
        if (emplaced) {
          state.manifest_dirty = true;
        }
        auto &app_ent = it->second;
        const auto common = vdf.childs.find("common");
        if (common != vdf.childs.cend()) {
          const auto name = common->second->attribs.find("name");
          if (name != common->second->attribs.cend()) {
            app_ent.name = std::move(name->second);
          }
        }
        for (auto depot_id : depot_ids) {
          const auto [it, emplaced] = app_ent.depots.try_emplace(depot_id);
          if (emplaced) {
            state.manifest_dirty = true;
          }
          auto &depot_ent = it->second;
          if (emplaced || !std::ranges::contains(depot_ent.accs, &acc)) {
            depot_ent.accs.emplace_back(&acc);
            depot_ent.next_acc = depot_ent.accs.cbegin();
          }
          if (!state.depot_keys.contains(depot_id)) {
            missing_keys.emplace_back(app.id, depot_id);
          }
        }
      } // if (!depot_ids.empty())
    } // if (depots != vdf.childs.cend())
    std::free(app.data);
  } // for (const auto &app : apps)
  delete[] data_pics.app_entries;
  delete &data_pics;
  acc.depot_ids = {};
  if (state.cur_status.load(std::memory_order::relaxed) == status::running) {
    sync_manifest();
  } else if (!acc.ready) {
    acc.ready = true;
    if (const std::scoped_lock lock(state.manifest_mtx);
        ++state.num_ready_accs == static_cast<int>(state.accounts.size())) {
      sync_manifest();
      state.cur_status.store(status::running, std::memory_order::relaxed);
    }
  }
  if (missing_keys.empty()) {
    return;
  }
  std::ranges::sort(missing_keys);
  // Schedule depot decryption key requests
  acc.rem_dk_total = missing_keys.size();
  acc.depot_key_requests.reset(new tek_sc_cm_data_depot_key[acc.rem_dk_total]);
  for (int i = 0; i < acc.rem_dk_total; ++i) {
    auto &data_dk = acc.depot_key_requests[i];
    const auto &item = missing_keys[i];
    data_dk.app_id = item.first;
    data_dk.depot_id = item.second;
  }
  // Send requests in bursts of 5, too many requests at once result in server
  //    refusing to process some of them, and timeouts. This will still happen
  //    even with such bursts, but nevertheless it's the faster way to get them
  {
    const int burst_size = std::min(5, acc.rem_dk_total);
    acc.rem_dk_burst = burst_size;
    for (int i = 0; i < burst_size; ++i) {
      tek_sc_cm_get_depot_key(client,
                              &acc.depot_key_requests[--acc.rem_dk_total],
                              cb_depot_key, 3000);
    }
  }
  return;
app_err:
  std::ranges::for_each(apps, std::free, &tek_sc_cm_pics_entry::data);
  delete[] data_pics.app_entries;
  delete &data_pics;
  tek_sc_cm_disconnect(client);
}

/// The callback for CM client PICS access tokens received event.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in, out] data
///    Pointer to `tek_sc_cm_data_pics` associated with the request.
/// @param [in, out] user_data
///    Pointer to the @ref account object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_write, 2),
  access(read_write, 3)]]
static void cb_access_tokens(tek_sc_cm_client *_Nonnull client,
                             void *_Nonnull data, void *_Nonnull user_data) {
  auto &data_pics = *reinterpret_cast<tek_sc_cm_data_pics *>(data);
  auto &acc = *reinterpret_cast<account *>(user_data);
  if (!tek_sc_err_success(&data_pics.result)) {
    std::println(std::cerr,
                 "Failed to get PICS access tokens for account {}'s apps:",
                 acc.token_info.steam_id);
    print_err(data_pics.result);
    delete[] data_pics.app_entries;
    delete &data_pics;
    tek_sc_cm_disconnect(client);
    return;
  }
  for (auto &app :
       std::span(data_pics.app_entries, data_pics.num_app_entries)) {
    if (tek_sc_err_success(&app.result)) {
      continue;
    }
    // Some apps are just weird and don't provide an access token.
    if (app.result.type == TEK_SC_ERR_TYPE_sub &&
        app.result.auxiliary == TEK_SC_ERRC_cm_access_token_denied) {
      app.access_token = 0;
      continue;
    }
    std::println(
        std::cerr,
        "Failed to get PICS access token for app {} owned by account {}:",
        app.id, acc.token_info.steam_id);
    print_err(app.result);
    delete[] data_pics.app_entries;
    delete &data_pics;
    tek_sc_cm_disconnect(client);
    return;
  }
  tek_sc_cm_get_product_info(client, &data_pics, cb_app_info, 10000);
}

/// The callback for CM client PICS package info received event.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in, out] data
///    Pointer to `tek_sc_cm_data_pics` associated with the request.
/// @param [in, out] user_data
///    Pointer to the @ref account object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_write, 2),
  access(read_write, 3)]]
static void cb_package_info(tek_sc_cm_client *_Nonnull client,
                            void *_Nonnull data, void *_Nonnull user_data) {
  auto &data_pics = *reinterpret_cast<tek_sc_cm_data_pics *>(data);
  auto &acc = *reinterpret_cast<account *>(user_data);
  if (!tek_sc_err_success(&data_pics.result)) {
    std::println(std::cerr,
                 "Failed to get PICS info for account {}'s packages:",
                 acc.token_info.steam_id);
    print_err(data_pics.result);
    delete[] data_pics.package_entries;
    delete &data_pics;
    tek_sc_cm_disconnect(client);
    return;
  }
  std::set<std::uint32_t> owned_app_ids;
  const std::span packages(data_pics.package_entries,
                           data_pics.num_package_entries);
  for (const auto &package : packages) {
    if (tek_sc_err_success(&package.result)) {
      continue;
    }
    std::println(std::cerr,
                 "Failed to get PICS info for package {} owned by account {}:",
                 package.id, acc.token_info.steam_id);
    print_err(package.result);
    std::ranges::for_each(packages, std::free, &tek_sc_cm_pics_entry::data);
    delete[] data_pics.package_entries;
    delete &data_pics;
    tek_sc_cm_disconnect(client);
    return;
  }
  for (const auto &package : packages) {
    auto cur = reinterpret_cast<const char *>(package.data);
    bin_vdf_node bvdf(cur, cur + package.data_size);
    if (const auto depot_ids = bvdf.children.find("depotids");
        depot_ids != bvdf.children.end()) {
      for (int depot_id : depot_ids->second->int_attrs | std::views::values) {
        acc.depot_ids.emplace(static_cast<std::uint32_t>(depot_id));
      };
    }
    if (const auto app_ids = bvdf.children.find("appids");
        app_ids != bvdf.children.end()) {
      for (int app_id : app_ids->second->int_attrs | std::views::values) {
        owned_app_ids.emplace(static_cast<std::uint32_t>(app_id));
        acc.depot_ids.emplace(static_cast<std::uint32_t>(app_id));
      }
    }
    std::free(package.data);
  }
  delete[] data_pics.package_entries;
  data_pics.num_package_entries = 0;
  data_pics.app_entries = new tek_sc_cm_pics_entry[owned_app_ids.size()]();
  data_pics.num_app_entries = owned_app_ids.size();
  for (auto &&[id, entry] :
       std::views::zip(owned_app_ids, std::span(data_pics.app_entries,
                                                owned_app_ids.size()))) {
    entry.id = id;
  }
  tek_sc_cm_get_access_token(client, &data_pics, cb_access_tokens, 10000);
  return;
}

/// The callback for CM client got licenses event.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in] data
///    Pointer to `tek_sc_cm_data_lics` providing the result.
/// @param [in, out] user_data
///    Pointer to the @ref account object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_only, 2),
  access(read_write, 3)]]
static void cb_lics(tek_sc_cm_client *_Nonnull client, void *_Nonnull data,
                    void *_Nonnull user_data) {
  const auto &data_lics = *reinterpret_cast<const tek_sc_cm_data_lics *>(data);
  auto &acc = *reinterpret_cast<account *>(user_data);
  if (!tek_sc_err_success(&data_lics.result)) {
    std::println(std::cerr, "Failed to get licenses for account {}:",
                 acc.token_info.steam_id);
    print_err(data_lics.result);
    tek_sc_cm_disconnect(client);
    return;
  }
  if (!data_lics.num_entries) {
    return;
  }
  auto &data_pics = *new tek_sc_cm_data_pics{
      .app_entries = nullptr,
      .package_entries = new tek_sc_cm_pics_entry[data_lics.num_entries](),
      .num_app_entries = 0,
      .num_package_entries = data_lics.num_entries,
      .timeout_ms = 10000,
      .result = {}};
  for (auto &&[lics_entry, pics_entry] :
       std::views::zip(std::span(data_lics.entries, data_lics.num_entries),
                       std::span(data_pics.package_entries,
                                 data_pics.num_package_entries))) {
    pics_entry.id = lics_entry.package_id;
    pics_entry.access_token = lics_entry.access_token;
  }
  tek_sc_cm_get_product_info(client, &data_pics, cb_package_info, 10000);
}

/// The callback for CM client signed in event.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in] data
///    Pointer to `tek_sc_err` indicating the result of the sign-in attempt.
/// @param [in, out] user_data
///    Pointer to the @ref account object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_only, 2),
  access(read_write, 3)]]
static void cb_signed_in(tek_sc_cm_client *_Nonnull client, void *_Nonnull data,
                         void *_Nonnull user_data) {
  const auto &res = *reinterpret_cast<const tek_sc_err *>(data);
  auto &acc = *reinterpret_cast<account *>(user_data);
  if (tek_sc_err_success(&res)) {
    tek_sc_cm_get_licenses(client, cb_lics, 10000);
    return;
  }
  if (res.type == TEK_SC_ERR_TYPE_steam_cm &&
      (res.auxiliary == TEK_SC_CM_ERESULT_access_denied ||
       res.auxiliary == TEK_SC_CM_ERESULT_invalid_signature)) {
    std::println("Auth token for account {} has been invalidated, removing it",
                 acc.token_info.steam_id);
    std::unique_lock lock(state.manifest_mtx);
    acc.rem_status.store(remove_status::pending_remove,
                         std::memory_order::relaxed);
    state.state_dirty = true;
    if (state.cur_status.load(std::memory_order::relaxed) == status::setup) {
      if (state.num_ready_accs == static_cast<int>(state.accounts.size()) - 1) {
        sync_manifest();
        state.cur_status.store(status::running, std::memory_order::relaxed);
      }
    } else {
      for (auto &app : state.apps | std::views::values) {
        for (auto &depot : app.depots | std::views::values) {
          if (std::erase(depot.accs, &acc)) {
            depot.next_acc = depot.accs.cbegin();
          }
        }
        if (std::erase_if(app.depots, [](const auto &pair) {
              return pair.second.accs.empty();
            })) {
          state.manifest_dirty = true;
        }
      }
      if (std::erase_if(state.apps, [](const auto &app) {
            return app.second.depots.empty();
          })) {
        state.manifest_dirty = true;
      }
      update_manifest();
    }
    lock.unlock();
    lws_cancel_service(state.lws_ctx);
  } else if (res.type != TEK_SC_ERR_TYPE_steam_cm ||
             res.auxiliary != TEK_SC_CM_ERESULT_service_unavailable) {
    std::println(std::cerr,
                 "Failed to sign into account {}:", acc.token_info.steam_id);
    print_err(res);
    state.exit_code = EXIT_FAILURE;
    state.cur_status.store(status::stopping, std::memory_order::relaxed);
    lws_context_destroy(state.lws_ctx);
  }
  tek_sc_cm_disconnect(client);
  return;
}

/// Send account token renew request.
///
/// @param [in] sul
///    Pointer to the scheduling element.
[[using gnu: nonnull(1), access(read_only, 1)]]
static void renew(lws_sorted_usec_list_t *_Nonnull sul);

/// The callback for CM authentication token renewed event.
///
/// @param [in, out] client
///    Pointer to the CM client instance that emitted the callback.
/// @param [in] data
///    Pointer to `tek_sc_cm_data_renew_roken` providing the result.
/// @param [in, out] user_data
///    Pointer to the @ref account object associated with @p client.
[[using gnu: nonnull(1, 2, 3), access(read_write, 1), access(read_only, 2),
  access(read_write, 3)]]
static void cb_token_renewed(tek_sc_cm_client *_Nonnull client,
                             void *_Nonnull data, void *_Nonnull user_data) {
  const auto &data_renew =
      *reinterpret_cast<const tek_sc_cm_data_renew_token *>(data);
  auto &acc = *reinterpret_cast<account *>(user_data);
  if (tek_sc_err_success(&data_renew.result)) {
    if (data_renew.new_token) {
      const auto token_info = tek_sc_cm_parse_auth_token(data_renew.new_token);
      if (token_info.steam_id) {
        acc.token = data_renew.new_token;
        acc.token_info = token_info;
        state.manifest_mtx.lock();
        state.state_dirty = true;
        update_manifest();
        state.manifest_mtx.unlock();
        std::println("Renewed auth token for account {}", token_info.steam_id);
        // Schedule the next renewal job
        acc.sul.cb = renew;
        acc.sul.us = lws_now_usecs() + (acc.token_info.expires - 7 * 24 * 3600 -
                                        std::chrono::system_clock::to_time_t(
                                            std::chrono::system_clock::now())) *
                                           LWS_USEC_PER_SEC;
        acc.ren_status = renew_status::pending_schedule;
        lws_cancel_service(state.lws_ctx);
      }
    }
  } else {
    std::println(std::cerr, "Failed to renew token for account {}:",
                 acc.token_info.steam_id);
    print_err(data_renew.result);
  }
  tek_sc_cm_disconnect(client);
}

static void renew(lws_sorted_usec_list_t *sul) {
  auto &acc = *reinterpret_cast<account *>(sul);
  tek_sc_cm_auth_renew_token(acc.cm_client, acc.token.data(), cb_token_renewed,
                             5000);
}

} // namespace

//===-- Internal functions ------------------------------------------------===//

void cb_connected(tek_sc_cm_client *client, void *data, void *user_data) {
  const auto &res = *reinterpret_cast<const tek_sc_err *>(data);
  auto &acc = *reinterpret_cast<account *>(user_data);
  if (!tek_sc_err_success(&res)) {
    std::println(std::cerr, "Failed to connect to a Steam CM server:");
    print_err(res);
    state.exit_code = EXIT_FAILURE;
    state.cur_status.store(status::stopping, std::memory_order::relaxed);
    lws_context_destroy(state.lws_ctx);
    return;
  }
  state.num_cm_connections.fetch_add(1, std::memory_order::relaxed);
  if (!acc.token_info.renewable) {
    tek_sc_cm_sign_in(client, acc.token.data(), cb_signed_in, 5000);
    return;
  }
  if (const auto now = std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now()),
      week_bef_exp = acc.token_info.expires - 7 * 24 * 3600;
      now < week_bef_exp) {
    // Schedule token renewal job
    acc.sul.cb = renew;
    acc.sul.us = lws_now_usecs() + (week_bef_exp - now) * LWS_USEC_PER_SEC;
    acc.ren_status = renew_status::pending_schedule;
    lws_cancel_service(state.lws_ctx);
    tek_sc_cm_sign_in(client, acc.token.data(), cb_signed_in, 5000);
  } else {
    // Less than a week left until token expiration, try renewing it
    tek_sc_cm_auth_renew_token(client, acc.token.data(), cb_token_renewed,
                               5000);
  }
}

void cb_disconnected(tek_sc_cm_client *client, void *data, void *user_data) {
  const auto &res = *reinterpret_cast<const tek_sc_err *>(data);
  auto &acc = *reinterpret_cast<account *>(user_data);
  if (state.num_cm_connections.fetch_sub(1, std::memory_order::relaxed) == 1) {
    ts3_os_futex_wake(&state.num_cm_connections);
  }
  if (!tek_sc_err_success(&res)) {
    std::println(std::cerr, "Abnormal disconnection from a Steam CM server:");
    print_err(res);
  }
  if (remove_status cur_status = remove_status::pending_remove;
      !acc.rem_status.compare_exchange_strong(cur_status, remove_status::remove,
                                              std::memory_order::relaxed,
                                              std::memory_order::relaxed) &&
      cur_status == remove_status::none &&
      state.cur_status.load(std::memory_order::relaxed) != status::stopping) {
    tek_sc_cm_connect(client, cb_connected, 5000, cb_disconnected);
  }
}

} // namespace tek::s3
