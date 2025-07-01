//===-- os_linux.c - GNU/Linux OS functions implementation ----------------===//
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
/// GNU/Linux implementation of @ref os.h.
///
//===----------------------------------------------------------------------===//
#include "os.h"

#include "null_attrs.h" // IWYU pragma: keep

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pwd.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <tek-steamclient/os.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>

//===-- Private functions -------------------------------------------------===//

/// Get user home directory path from their passwd entry.
///
/// @param [out] buf
///    Address of variable that receives pointer to the heap-allocated buffer
///    storing the passwd data on success. It must be freed with `free` after
///    use.
/// @return Home directory path, or `nullptr` on failure.
[[gnu::nonnull(1), gnu::access(write_only, 1)]] static inline char
    *_Nullable ts3_get_pw_dir(void *_Nullable *_Nonnull buf) {
  auto buf_size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (buf_size <= 0) {
    buf_size = 1024;
  }
  auto pw_buf = malloc(buf_size);
  if (!pw_buf) {
    return nullptr;
  }
  for (auto const euid = geteuid();;) {
    struct passwd pw;
    struct passwd *pw_res;
    const int res = getpwuid_r(euid, &pw, pw_buf, buf_size, &pw_res);
    if (res == ERANGE) {
      // Buffer is too small, double it and try again
      buf_size *= 2;
      free(pw_buf);
      pw_buf = malloc(buf_size);
      if (!pw_buf) {
        return nullptr;
      }
      continue;
    }
    if (res != 0 || !pw_res) {
      // An error has occurred
      free(pw_buf);
      return nullptr;
    }
    // Successfully got the passwd entry
    *buf = pw_buf;
    return pw.pw_dir;
  }
}

/// Get path from an XDG environment variable.
///
/// @param [in] var_name
///    Name of the XDG variable, as a null-terminated string.
/// @return Heap-allocated copy of path extracted from the variable,
///    `nullptr` if there was none.
[[gnu::nonnull(1), gnu::access(read_only, 1),
  gnu::null_terminated_string_arg(1)]] static inline char
    *_Nullable ts3_get_xdg_var(const char *_Nonnull var_name) {
  auto const val = secure_getenv(var_name);
  if (!val || !val[0]) {
    return nullptr;
  }
  // Expand the value since it may contain other variables e.g. $HOME
  wordexp_t we;
  if (wordexp(val, &we, WRDE_NOCMD) != 0) {
    return nullptr;
  }
  if (!we.we_wordc) {
    wordfree(&we);
    return nullptr;
  }
  auto const path = strdup(we.we_wordv[0]);
  wordfree(&we);
  return path;
}

//===-- Public functions --------------------------------------------------===//

//===-- General functions -------------------------------------------------===//

void ts3_os_close_handle(tek_sc_os_handle handle) { close(handle); }

tek_sc_os_char *ts3_os_get_config_dir(void) {
  // Try getting value of $XDG_CONFIG_HOME first
  auto const xdg_config_home = ts3_get_xdg_var("XDG_CONFIG_HOME");
  if (xdg_config_home) {
    return xdg_config_home;
  }
  // Otherwise use "/etc" for root, or try "$HOME/.config"
  if (geteuid() == 0) {
    return strdup("/etc");
  }
  auto const home = secure_getenv("HOME");
  static const char rel_path[] = "/.config";
  if (home && home[0]) {
    auto const path_size = strlen(home) + sizeof rel_path;
    char *const path = malloc(path_size);
    if (!path) {
      return nullptr;
    }
    strlcpy(path, home, path_size);
    strlcat(path, rel_path, path_size);
    return path;
  }
  // If even $HOME is not set, fallback to home directory from passwd entry of
  //    current user
  void *buf;
  auto const pw_dir = ts3_get_pw_dir(&buf);
  if (!pw_dir) {
    return nullptr;
  }
  auto const path_size = strlen(pw_dir) + sizeof rel_path;
  char *const path = malloc(path_size);
  if (!path) {
    free(buf);
    return nullptr;
  }
  strlcpy(path, pw_dir, path_size);
  free(buf);
  strlcat(path, rel_path, path_size);
  return path;
}

char *ts3_os_get_err_msg(tek_sc_os_errc errc) {
  char *const buf = malloc(256);
  if (!buf) {
    abort();
  }
  auto const res = strerror_r(errc, buf, 256);
  if (res != buf) {
    strlcpy(buf, res, 256);
  }
  return buf;
}

void ts3_os_get_hostname(char *buf, int buf_size) {
  if (buf_size <= 0) {
    return;
  }
  if (gethostname(buf, buf_size) < 0) {
    buf[buf_size - 1] = '\0';
  }
}

tek_sc_os_errc ts3_os_get_last_error(void) { return errno; }

tek_sc_os_char *ts3_os_get_state_dir(void) {
  // Try getting value of $XDG_STATE_HOME first
  auto const xdg_state_home = ts3_get_xdg_var("XDG_STATE_HOME");
  if (xdg_state_home) {
    return xdg_state_home;
  }
  // Otherwise use "/var/lib" for root, or try "$HOME/.local/state"
  if (geteuid() == 0) {
    return strdup("/var/lib");
  }
  auto const home = secure_getenv("HOME");
  static const char rel_path[] = "/.local/state";
  if (home) {
    auto const path_size = strlen(home) + sizeof rel_path;
    char *const path = malloc(path_size);
    if (!path) {
      return nullptr;
    }
    strlcpy(path, home, path_size);
    strlcat(path, rel_path, path_size);
    return path;
  }
  // If even $HOME is not set, fallback to home directory from passwd entry of
  //    current user
  void *buf;
  auto const pw_dir = ts3_get_pw_dir(&buf);
  if (!pw_dir) {
    return nullptr;
  }
  auto const path_size = strlen(pw_dir) + sizeof rel_path;
  char *const path = malloc(path_size);
  if (!path) {
    free(buf);
    return nullptr;
  }
  strlcpy(path, pw_dir, path_size);
  free(buf);
  strlcat(path, rel_path, path_size);
  return path;
}

//===-- I/O functions -----------------------------------------------------===//

//===--- Diectory create/open ---------------------------------------------===//

tek_sc_os_handle ts3_os_dir_create(const tek_sc_os_char *path) {
  const int fd = open(path, O_DIRECTORY | O_CLOEXEC | O_PATH);
  if (fd >= 0) {
    return fd;
  }
  if (errno != ENOENT) {
    return -1;
  }
  if (mkdir(path, 0755) < 0) {
    return -1;
  }
  return open(path, O_DIRECTORY | O_CLOEXEC | O_PATH);
}

tek_sc_os_handle ts3_os_dir_create_at(tek_sc_os_handle parent_dir_handle,
                                      const tek_sc_os_char *name) {
  const int fd =
      openat(parent_dir_handle, name, O_DIRECTORY | O_CLOEXEC | O_PATH);
  if (fd >= 0) {
    return fd;
  }
  if (errno != ENOENT) {
    return -1;
  }
  if (mkdirat(parent_dir_handle, name, 0755) < 0) {
    return -1;
  }
  return openat(parent_dir_handle, name, O_DIRECTORY | O_CLOEXEC | O_PATH);
}

//===--- File create/open -------------------------------------------------===//

tek_sc_os_handle ts3_os_file_create_at(tek_sc_os_handle parent_dir_handle,
                                       const tek_sc_os_char *name) {
  return openat(parent_dir_handle, name,
                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
}

tek_sc_os_handle ts3_os_file_open(const tek_sc_os_char *path) {
  return open(path, O_RDONLY | O_CLOEXEC);
}

//===--- File read/write --------------------------------------------------===//

bool ts3_os_file_read(tek_sc_os_handle handle, void *buf, size_t n) {
  for (;;) {
    auto const bytes_read = read(handle, buf, n);
    if (bytes_read < 0) {
      return false;
    }
    n -= bytes_read;
    if (!n) {
      return true;
    }
    if (!bytes_read) {
      // Intentionally picked an errno value unused by read(), this branch helps
      //     avoiding a deadlock if early EOF is encountered
      errno = ERANGE;
      return false;
    }
    buf += bytes_read;
  }
}

bool ts3_os_file_write(tek_sc_os_handle handle, const void *buf, size_t n) {
  for (;;) {
    auto const bytes_written = write(handle, buf, n);
    if (bytes_written < 0) {
      return false;
    }
    n -= bytes_written;
    if (!n) {
      return true;
    }
    if (!bytes_written) {
      // Intentionally picked an errno value unused by write(), this branch
      //    helps avoiding a deadlock if early EOF is encountered
      errno = ERANGE;
      return false;
    }
    buf += bytes_written;
  }
}

//===--- File get size ----------------------------------------------------===//

size_t ts3_os_file_get_size(tek_sc_os_handle handle) {
  struct statx stx;
  if (statx(handle, "", AT_EMPTY_PATH, STATX_SIZE, &stx) < 0) {
    return SIZE_MAX;
  }
  if (!(stx.stx_mask & STATX_SIZE)) {
    errno = EINVAL;
    return SIZE_MAX;
  }
  return stx.stx_size;
}

//===-- Futex functions ---------------------------------------------------===//

bool ts3_os_futex_wait(const _Atomic(uint32_t) *addr, uint32_t old,
                       uint32_t timeout_ms) {
  const struct timespec ts = {.tv_sec = timeout_ms / 1000,
                              .tv_nsec = (timeout_ms % 1000) * 1000000};
  do {
    if (syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, old, &ts) < 0) {
      return errno == EAGAIN;
    }
  } while (atomic_load_explicit(addr, memory_order_relaxed) == old);
  return true;
}

void ts3_os_futex_wake(_Atomic(uint32_t) *addr) {
  syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 1);
}
