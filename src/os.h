//===-- os.h - OS-specific code -------------------------------------------===//
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
/// Declarations of macros, types and functions that are implemented
///    differently on different operating systems. Implementations are provided
///    by corresponding os_*.c.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "null_attrs.h" // IWYU pragma: keep

#include <libwebsockets.h>
#include <stdatomic.h> // IWYU pragma: keep
#include <stddef.h>
#include <stdint.h>
#include <tek-steamclient/os.h>

//===-- Types and macros --------------------------------------------------===//

#ifdef _WIN32

/// @def TS3_OS_ERR_FILE_NOT_FOUND
/// `tek_sc_os_errc` value indicating that a file was not found.
#define TS3_OS_ERR_FILE_NOT_FOUND ERROR_PATH_NOT_FOUND
/// @def TS3_OS_INVALID_HANDLE
/// Invalid value for `tek_sc_os_handle`.
#define TS3_OS_INVALID_HANDLE INVALID_HANDLE_VALUE
/// @def TS3_OS_PATH_SEP_CHAR_STR
/// Path separator character for current operating system as a string literal.
#define TS3_OS_PATH_SEP_CHAR_STR "\\"

#elifdef __linux__ // def _WIN32

#include <errno.h>

/// @def TS3_OS_ERR_FILE_NOT_FOUND
/// `tek_sc_os_errc` value indicating that a file was not found.
#define TS3_OS_ERR_FILE_NOT_FOUND ENOENT
/// @def TS3_OS_INVALID_HANDLE
/// Invalid value for `tek_sc_os_handle`.
#define TS3_OS_INVALID_HANDLE -1
/// @def TS3_OS_PATH_SEP_CHAR_STR
/// Path separator character for current operating system as a string literal.
#define TS3_OS_PATH_SEP_CHAR_STR "/"

#endif // def _WIN32 elifdef __linux__

//===-- Functions ---------------------------------------------------------===//

#ifdef __cplusplus
extern "C" {
#endif // def __cplusplus

//===-- General functions -------------------------------------------------===//

/// Close operating system resource handle.
///
/// @param handle
///    OS handle to close.
[[gnu::visibility("internal"), gnu::fd_arg(1)]]
void ts3_os_close_handle(
    [[clang::release_handle("os")]] tek_sc_os_handle handle);

/// Get path to the config directory for current user.
///
/// @return Path to the config directory for current user, as a heap-allocated
///    null-terminated string, or `nullptr` on failure. The returned pointer
///    must be freed with `free` after use.
[[gnu::visibility("internal")]] tek_sc_os_char *_Nullable ts3_os_get_config_dir(
    void);

/// Get the message for specified error code.
///
/// @param errc
///    OS error code to get the message for.
/// @return Human-readable message for @p errc, as a heap-allocated
///    null-terminated UTF-8 string. It must be freed with `free` after use.
[[gnu::visibility("internal"), gnu::returns_nonnull]]
char *_Nonnull ts3_os_get_err_msg(tek_sc_os_errc errc);

/// Get system's hostname.
///
/// @param [out] buf
///    Pointer to the buffer that receives null-terminated UTF-8 hostname
///    string.
/// @param buf_size
///    Size of the buffer pointed to by @p buf.
[[gnu::visibility("internal"), gnu::nonnull(1), gnu::access(write_only, 1, 2)]]
void ts3_os_get_hostname(char *_Nonnull buf, int buf_size);

/// Get the last error code set by a system call.
///
/// @return OS-specific error code.
[[gnu::visibility("internal")]]
tek_sc_os_errc ts3_os_get_last_error(void);

/// Get path to the program state directory for current user.
///
/// @return Path to the program state directory for current user, as a
///    heap-allocated null-terminated string, or `nullptr` on failure. The
///    returned pointer must be freed with `free` after use.
[[gnu::visibility("internal")]] tek_sc_os_char *_Nullable ts3_os_get_state_dir(
    void);

//===-- I/O functions -----------------------------------------------------===//

//===--- Diectory create/open ---------------------------------------------===//

/// Open a directory, or create it if it doesn't exist.
///
/// @param [in] path
///    Path to the directory to open/create, as a null-terminated string.
/// @return Handle for the opened directory, or @ref TS3_OS_INVALID_HANDLE if
///    the function fails. Use @ref ts3_os_get_last_error to get the error
///    code. The returned handle must be closed with @ref ts3_os_close_handle
///    after use.
[[gnu::visibility("internal"), gnu::nonnull(1), gnu::access(read_only, 1),
  gnu::null_terminated_string_arg(1), clang::acquire_handle("os")]]
tek_sc_os_handle ts3_os_dir_create(const tek_sc_os_char *_Nonnull path);

/// Open a subdirectory at specified directory, or create it if it doesn't
///    exist.
///
/// @param parent_dir_handle
///    Handle for the parent directory.
/// @param [in] name
///    Name of the subdirectory to open/create, as a null-terminated string.
/// @return Handle for the opened subdirectory, or @ref TS3_OS_INVALID_HANDLE
///    if the function fails. Use @ref ts3_os_get_last_error to get the error
///    code. The returned handle must be closed with @ref ts3_os_close_handle
///    after use.
[[gnu::visibility("internal"), gnu::fd_arg(1), gnu::nonnull(2),
  gnu::access(read_only, 2), gnu::null_terminated_string_arg(2),
  clang::acquire_handle("os")]]
tek_sc_os_handle ts3_os_dir_create_at(
    [[clang::use_handle("os")]] tek_sc_os_handle parent_dir_handle,
    const tek_sc_os_char *_Nonnull name);

//===--- File create/open -------------------------------------------------===//

/// Open a file at specified directory for writing, or create it if it doesn't
///    exist, and truncate it to 0 bytes.
///
/// @param parent_dir_handle
///    Handle for the parent directory of the file.
/// @param [in] name
///    Name of the file to open/create, as a null-terminated string.
/// @return Handle for the opened file, or @ref TS3_OS_INVALID_HANDLE if the
///    function fails. Use @ref ts3_os_get_last_error to get the error code. The
///    returned handle must be closed with @ref ts3_os_close_handle after use.
[[gnu::visibility("internal"), gnu::fd_arg(1), gnu::nonnull(2),
  gnu::access(read_only, 2), gnu::null_terminated_string_arg(2),
  clang::acquire_handle("os")]]
tek_sc_os_handle ts3_os_file_create_at(
    [[clang::use_handle("os")]] tek_sc_os_handle parent_dir_handle,
    const tek_sc_os_char *_Nonnull name);

/// Open a file for reading.
///
/// @param [in] path
///    Path to the file to open, as a null-terminated string.
/// @return Handle for the opened file, or @ref TS3_OS_INVALID_HANDLE if the
///    function fails. Use @ref ts3_os_get_last_error to get the error code. The
///    returned handle must be closed with @ref ts3_os_close_handle after use.
[[gnu::visibility("internal"), gnu::nonnull(1), gnu::access(read_only, 1),
  gnu::null_terminated_string_arg(1), clang::acquire_handle("os")]]
tek_sc_os_handle ts3_os_file_open(const tek_sc_os_char *_Nonnull path);

//===--- File read/write --------------------------------------------------===//

/// Read data from file. Exactly @p n bytes will be read.
///
/// @param handle
///    OS handle for the file.
/// @param [out] buf
///    Pointer to the buffer that receives the read data.
/// @param n
///    Number of bytes to read.
/// @return Value indicating whether the function succeeded. Use
///    @ref ts3_os_get_last_error to get the error code in case of failure.
[[gnu::visibility("internal"), gnu::fd_arg_read(1), gnu::nonnull(2),
  gnu::access(write_only, 2, 3)]]
bool ts3_os_file_read([[clang::use_handle("os")]] tek_sc_os_handle handle,
                      void *_Nonnull buf, size_t n);

/// Write data to file. Exactly @p n bytes will be written.
///
/// @param handle
///    OS handle for the file.
/// @param [in] buf
///    Pointer to the buffer containing the data to write.
/// @param n
///    Number of bytes to write.
/// @return Value indicating whether the function succeeded. Use
///    @ref ts3_os_get_last_error to get the error code in case of failure.
[[gnu::visibility("internal"), gnu::fd_arg_write(1), gnu::nonnull(2),
  gnu::access(read_only, 2, 3)]]
bool ts3_os_file_write([[clang::use_handle("os")]] tek_sc_os_handle handle,
                       const void *_Nonnull buf, size_t n);

//===--- File get size ----------------------------------------------------===//

/// Get the size of file.
///
/// @param handle
///    OS handle for the file.
/// @return Size of the file in bytes, or `SIZE_MAX` if the function fails. Use
///    @ref ts3_os_get_last_error to get the error code.
[[gnu::visibility("internal"), gnu::fd_arg_read(1)]]
size_t
ts3_os_file_get_size([[clang::use_handle("os")]] tek_sc_os_handle handle);

//===-- Futex functions ---------------------------------------------------===//

/// Wait for a value at @p addr to change from @p old.
///
/// @param [in] addr
///    Pointer to the value to await change for.
/// @param old
///    Value at @p addr that triggers the wait.
/// @param timeout_ms
///    Timeout of the wait operation, in milliseconds.
/// @return Value indicating whether the wait succeeded, `false` on timeout.
[[gnu::visibility("internal"), gnu::nonnull(1), gnu::access(read_only, 1)]]
bool ts3_os_futex_wait(const _Atomic(uint32_t) *_Nonnull addr, uint32_t old,
                       uint32_t timeout_ms);

/// Wake the thread waiting on specified address.
///
/// @param addr
///    Address of the futex to wake the thread for.
[[gnu::visibility("internal"), gnu::nonnull(1), gnu::access(none, 1)]]
void ts3_os_futex_wake(_Atomic(uint32_t) *_Nonnull addr);

#ifdef __cplusplus
} // extern "C"

namespace tek::s3 {

/// RAII wrapper for `tek_sc_os_handle`.
struct [[gnu::visibility("internal")]] os_handle {
  tek_sc_os_handle value;

  constexpr os_handle(tek_sc_os_handle handle) noexcept : value(handle) {}
  constexpr ~os_handle() noexcept { close(); }

  constexpr operator bool() const noexcept {
    return value != TS3_OS_INVALID_HANDLE;
  }
  constexpr void close() noexcept {
    if (value != TS3_OS_INVALID_HANDLE) {
      ts3_os_close_handle(value);
      value = TS3_OS_INVALID_HANDLE;
    }
  }
};

} // namespace tek::s3

#endif // def __cplusplus
