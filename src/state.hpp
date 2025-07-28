//===-- state.hpp - Global program state declarations ---------------------===//
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
/// Declarations of program state types, functions, and global state instance.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "config.h"     // IWYU pragma: keep
#include "null_attrs.h" // IWYU pragma: keep
#include "signin.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <libwebsockets.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tek-steamclient/base.h>
#include <tek-steamclient/cm.h>
#include <vector>

namespace tek::s3 {

/// Maximum size of a response packet.
constexpr std::size_t tx_size = 32768;

/// Global program status values.
enum class status {
  /// First account sign-ins and initial manifest generation are being
  ///    performed.
  setup,
  /// The server is running normally.
  running,
  /// A stop request has been received
  stopping
};

/// Account remove status values.
enum class remove_status {
  /// The account removal is not scheduled.
  none,
  /// The account will be scheduled for removal after its CM client is
  ///    disconnected.
  pending_remove,
  /// Main thread should remove the account on next opportunity.
  remove
};

/// Account token renewal status values.
enum class renew_status {
  /// The token renewal job is not scheduled.
  not_scheduled,
  /// Main thread should schedule the renewal job on next opportunity.
  pending_schedule,
  /// The renewal job has been scheduled.
  scheduled
};

/// Steam account and CM client wrapper entry.
struct account {
  /// Doubly linked list element for libwebsockets renewal job scheduling.
  lws_sorted_usec_list_t sul;
  /// Pointer to the CM client instance associated with the account.
  tek_sc_cm_client *_Nullable cm_client;
  /// Authentication token.
  std::string token;
  /// Information parsed from @ref token.
  tek_sc_cm_auth_token_info token_info;
  /// Status of the token renewal job.
  renew_status ren_status;
  /// Total number of depot decryption keys remaining to be acquired.
  int rem_dk_total;
  /// Number of remaining depot decryption key responses in current burst.
  int rem_dk_burst;
  /// Value indicating whether the account should be removed.
  std::atomic<remove_status> rem_status;
  /// Pointer to the array of depot decryption key request/response data
  ///    structures.
  std::unique_ptr<tek_sc_cm_data_depot_key[]> depot_key_requests;
  /// IDs of depots owned by the acccount.
  std::set<std::uint32_t> depot_ids;
  /// Value indicating whether the application list for this account has been
  ///    received at least once.
  bool ready;
};

/// Steam depot entry.
struct depot {
  /// Pointers to accounts owning a license for the depot, which can be used to
  ///    provide manifest request codes.
  std::vector<account *> accs;
  /// Iterator pointing to the next account to get manifest request code with.
  decltype(accs)::const_iterator next_acc;
};

/// Steam application entry.
struct app {
  /// Name of the application.
  std::string name;
  /// Depots belonging to the application, by ID.
  std::map<std::uint32_t, depot> depots;
};

/// Manifest request code cache entry.
struct mrc_cache {
  /// Doubly linked list element for libwebsockets removal job scheduling.
  lws_sorted_usec_list_t sul;
  /// ID of the manifest, used by the removal callback to find the entry.
  std::uint64_t manifest_id;
  /// Manifest request code value.
  std::uint64_t mrc;
};

/// Reference-counted lock that locks associated mutex when its reference
///    counter is not zero.
class [[gnu::visibility("internal")]] ref_counted_lock {
  /// Mutex to lock when there are active references.
  std::recursive_mutex &mtx;
  /// Reference counter;
  int ref_count;

public:
  constexpr ref_counted_lock(std::recursive_mutex &mtx) noexcept
      : mtx(mtx), ref_count(0) {}
  /// Increment reference counter and lock the mutex if its previous value was
  /// zero.
  void lock() {
    if (!ref_count++) {
      mtx.lock();
    }
  }
  /// Decrement reference counter and unlock the mutex if its new value is zero.
  void unlock() {
    if (!--ref_count) {
      mtx.unlock();
    }
  }
  /// Unlock the mutex if it has active references, and reset the reference
  /// counter.
  void force_unlock() {
    if (ref_count) {
      mtx.unlock();
    }
    ref_count = 0;
  }
};

/// Wrapper around a buffer pointer with known size.
struct sized_buf {
  /// Pointer to the buffer.
  std::unique_ptr<unsigned char[]> buf;
  /// Size of the buffer pointed to by @ref buf, in bytes.
  std::size_t size;
};

/// tek-s3 program state.
struct ts3_state {
  /// Pointer to the libwebsockets context.
  lws_context *_Nonnull lws_ctx;
  /// Global program status.
  std::atomic<status> cur_status;
  /// Number of active CM server connections.
  std::atomic_uint32_t num_cm_connections;
  /// Mutex for locking concurrent access to all manifest-related fields.
  std::recursive_mutex manifest_mtx;
  /// Lock for @ref manifest_mtx during manifest downloads.
  ref_counted_lock download_lock{manifest_mtx};
  /// Timestamp (seconds since Epoch) of last manifest update.
  std::time_t timestamp;
  // Steam accounts that the server has access to, by Steam IDs.
  std::map<std::uint64_t, account> accounts;
  /// Steam applications owned by server's accounts.
  std::map<std::uint32_t, app> apps;
  /// Known AES-256 depot decryption keys.
  std::map<std::uint32_t, tek_sc_aes256_key> depot_keys;
  /// Pre-serialized manifest JSON.
  sized_buf manifest;
  /// @ref manifest pre-compressed with deflate.
  sized_buf manifest_deflate;
#ifdef TEK_S3B_BROTLI
  /// @ref manifest pre-compressed with brotli.
  sized_buf manifest_brotli;
#endif // TEK_S3B_BROTLI
#ifdef TEK_S3B_ZSTD
  /// @ref manifest pre-compressed with zstd.
  sized_buf manifest_zstd;
#endif // TEK_S3B_ZSTD
  /// Manifest request code cache.
  std::map<std::uint64_t, mrc_cache> mrcs;
  /// Pointers to active sign-in contexts.
  std::vector<signin_ctx *> signin_ctxs;
  /// Pointer to the tek-steamclient library context.
  tek_sc_lib_ctx *_Nonnull tek_sc_ctx;
  /// Number of accounts ready to process manifest request code requests.
  int num_ready_accs;
  /// Exit code of the program.
  int exit_code = EXIT_SUCCESS;
  /// Value indicating whether the manifest and its timestamp need to be
  ///    updated.
  bool manifest_dirty;
  /// Value indicating whether the state file needs to be updated.
  bool state_dirty;
};

/// tek-s3 libwebsockets protocol.
[[gnu::visibility("internal")]]
extern const lws_protocols protocol;

/// Global program state object.
[[gnu::visibility("internal")]]
inline ts3_state state;

/// Update pre-serialized and pre-compressed manifest buffers, the state file
///    and timestamp if necessary.
[[gnu::visibility("internal")]]
void update_manifest();

} // namespace tek::s3
