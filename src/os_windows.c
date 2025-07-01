//===-- os_windows.c - Windows OS functions implementation ----------------===//
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
/// Windows implementation of @ref os.h.
///
//===----------------------------------------------------------------------===//
#include "os.h"

#include "null_attrs.h" // IWYU pragma: keep

#include <shlobj.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tek-steamclient/os.h>
#include <wchar.h>
#include <winsock2.h>
#include <winternl.h>

//===-- Declarations missing from winternl.h ------------------------------===//

NTSTATUS NTAPI NtReadFile(HANDLE FileHandle, HANDLE Event,
                          PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                          PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
                          ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);

NTSTATUS NTAPI NtWriteFile(HANDLE FileHandle, HANDLE Event,
                           PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                           PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
                           ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);

//===-- Public functions --------------------------------------------------===//

//===-- General functions -------------------------------------------------===//

void ts3_os_close_handle(tek_sc_os_handle handle) { NtClose(handle); }

tek_sc_os_char *ts3_os_get_config_dir(void) {
  PWSTR path;
  if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr,
                           &path) != S_OK) {
    return nullptr;
  }
  auto const buf_size = (wcslen(path) + 1) * sizeof *path;
  WCHAR *const buf = malloc(buf_size);
  if (buf) {
    memcpy(buf, path, buf_size);
  }
  CoTaskMemFree(path);
  return buf;
}

char *ts3_os_get_err_msg(tek_sc_os_errc errc) {
  LPWSTR msg;
  auto const res = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                      FORMAT_MESSAGE_IGNORE_INSERTS |
                                      FORMAT_MESSAGE_FROM_SYSTEM,
                                  nullptr, errc, 0, (LPWSTR)&msg, 0, nullptr);
  if (!res) {
    static const char unk_msg[] = "Unknown error";
    char *const buf = malloc(sizeof unk_msg);
    if (!buf) {
      abort();
    }
    memcpy(buf, unk_msg, sizeof unk_msg);
    return buf;
  }
  auto const buf_size = WideCharToMultiByte(CP_UTF8, 0, msg, res + 1, nullptr,
                                            0, nullptr, nullptr);
  char *const buf = malloc(buf_size);
  if (!buf) {
    abort();
  }
  WideCharToMultiByte(CP_UTF8, 0, msg, res + 1, buf, buf_size, nullptr,
                      nullptr);
  LocalFree(msg);
  return buf;
}

void ts3_os_get_hostname(char *buf, int buf_size) {
  if (buf_size <= 0) {
    return;
  }
  if (gethostname(buf, buf_size) != 0) {
    buf[0] = '\0';
  }
}

tek_sc_os_errc ts3_os_get_last_error(void) { return GetLastError(); }

tek_sc_os_char *ts3_os_get_state_dir(void) {
  PWSTR path;
  if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                           &path) != S_OK) {
    return nullptr;
  }
  auto const buf_size = (wcslen(path) + 1) * sizeof *path;
  WCHAR *const buf = malloc(buf_size);
  if (buf) {
    memcpy(buf, path, buf_size);
  }
  CoTaskMemFree(path);
  return buf;
}

//===-- I/O functions -----------------------------------------------------===//

//===--- Diectory create/open ---------------------------------------------===//

tek_sc_os_handle ts3_os_dir_create(const tek_sc_os_char *path) {
  UNICODE_STRING path_str;
  if (!RtlDosPathNameToNtPathName_U(path, &path_str, nullptr, nullptr)) {
    SetLastError(ERROR_BAD_PATHNAME);
    return INVALID_HANDLE_VALUE;
  }
  IO_STATUS_BLOCK isb;
  HANDLE handle;
  auto const status = NtCreateFile(
      &handle, FILE_TRAVERSE,
      &(OBJECT_ATTRIBUTES){.Length = sizeof(OBJECT_ATTRIBUTES),
                           .ObjectName = &path_str,
                           .Attributes = OBJ_CASE_INSENSITIVE},
      &isb, nullptr, FILE_ATTRIBUTE_DIRECTORY, FILE_SHARE_VALID_FLAGS,
      FILE_OPEN_IF, FILE_DIRECTORY_FILE, nullptr, 0);
  RtlFreeUnicodeString(&path_str);
  if (NT_SUCCESS(status)) {
    return handle;
  }
  SetLastError(RtlNtStatusToDosError(status));
  return INVALID_HANDLE_VALUE;
}

tek_sc_os_handle ts3_os_dir_create_at(tek_sc_os_handle parent_dir_handle,
                                      const tek_sc_os_char *name) {
  const USHORT name_size = wcslen(name) * sizeof *name;
  IO_STATUS_BLOCK isb;
  HANDLE handle;
  auto const status = NtCreateFile(
      &handle, FILE_TRAVERSE,
      &(OBJECT_ATTRIBUTES){.Length = sizeof(OBJECT_ATTRIBUTES),
                           .RootDirectory = parent_dir_handle,
                           .ObjectName =
                               &(UNICODE_STRING){.Length = name_size,
                                                 .MaximumLength = name_size,
                                                 .Buffer = (PWSTR)name},
                           .Attributes = OBJ_CASE_INSENSITIVE},
      &isb, nullptr, FILE_ATTRIBUTE_DIRECTORY, FILE_SHARE_VALID_FLAGS,
      FILE_OPEN_IF, FILE_DIRECTORY_FILE, nullptr, 0);
  if (NT_SUCCESS(status)) {
    return handle;
  }
  SetLastError(RtlNtStatusToDosError(status));
  return INVALID_HANDLE_VALUE;
}

//===--- File create/open -------------------------------------------------===//

tek_sc_os_handle ts3_os_file_create_at(tek_sc_os_handle parent_dir_handle,
                                       const tek_sc_os_char *name) {
  const USHORT name_size = wcslen(name) * sizeof *name;
  IO_STATUS_BLOCK isb;
  HANDLE handle;
  auto const status = NtCreateFile(
      &handle, FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
      &(OBJECT_ATTRIBUTES){.Length = sizeof(OBJECT_ATTRIBUTES),
                           .RootDirectory = parent_dir_handle,
                           .ObjectName =
                               &(UNICODE_STRING){.Length = name_size,
                                                 .MaximumLength = name_size,
                                                 .Buffer = (PWSTR)name},
                           .Attributes = OBJ_CASE_INSENSITIVE},
      &isb, nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OVERWRITE_IF,
      FILE_SEQUENTIAL_ONLY | FILE_SYNCHRONOUS_IO_NONALERT |
          FILE_NON_DIRECTORY_FILE,
      nullptr, 0);
  if (NT_SUCCESS(status)) {
    return handle;
  }
  SetLastError(RtlNtStatusToDosError(status));
  return INVALID_HANDLE_VALUE;
}

tek_sc_os_handle ts3_os_file_open(const tek_sc_os_char *path) {
  UNICODE_STRING path_str;
  if (!RtlDosPathNameToNtPathName_U(path, &path_str, nullptr, nullptr)) {
    SetLastError(ERROR_BAD_PATHNAME);
    return INVALID_HANDLE_VALUE;
  }
  IO_STATUS_BLOCK isb;
  HANDLE handle;
  auto const status =
      NtOpenFile(&handle, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                 &(OBJECT_ATTRIBUTES){.Length = sizeof(OBJECT_ATTRIBUTES),
                                      .ObjectName = &path_str,
                                      .Attributes = OBJ_CASE_INSENSITIVE},
                 &isb, FILE_SHARE_READ,
                 FILE_SEQUENTIAL_ONLY | FILE_SYNCHRONOUS_IO_NONALERT |
                     FILE_NON_DIRECTORY_FILE);
  RtlFreeUnicodeString(&path_str);
  if (NT_SUCCESS(status)) {
    return handle;
  }
  SetLastError(RtlNtStatusToDosError(status));
  return INVALID_HANDLE_VALUE;
}

//===--- File read/write --------------------------------------------------===//

bool ts3_os_file_read(tek_sc_os_handle handle, void *buf, size_t n) {
  for (IO_STATUS_BLOCK isb;;) {
    auto const status = NtReadFile(handle, nullptr, nullptr, nullptr, &isb, buf,
                                   n, nullptr, nullptr);
    if (!NT_SUCCESS(status)) {
      SetLastError(RtlNtStatusToDosError(status));
      return false;
    }
    n -= isb.Information;
    if (!n) {
      return true;
    }
    if (!isb.Information) {
      SetLastError(ERROR_READ_FAULT);
      return false;
    }
    buf += isb.Information;
  }
}

bool ts3_os_file_write(tek_sc_os_handle handle, const void *buf, size_t n) {
  for (IO_STATUS_BLOCK isb;;) {
    auto const status = NtWriteFile(handle, nullptr, nullptr, nullptr, &isb,
                                    (PVOID)buf, n, nullptr, nullptr);
    if (!NT_SUCCESS(status)) {
      SetLastError(RtlNtStatusToDosError(status));
      return false;
    }
    n -= isb.Information;
    if (!n) {
      return true;
    }
    if (!isb.Information) {
      SetLastError(ERROR_WRITE_FAULT);
      return false;
    }
    buf += isb.Information;
  }
}

//===--- File get size ----------------------------------------------------===//

size_t ts3_os_file_get_size(tek_sc_os_handle handle) {
  IO_STATUS_BLOCK isb;
  FILE_STANDARD_INFORMATION info;
  auto const status = NtQueryInformationFile(handle, &isb, &info, sizeof info,
                                             FileStandardInformation);
  if (NT_SUCCESS(status)) {
    return (size_t)info.EndOfFile.QuadPart;
  }
  SetLastError(RtlNtStatusToDosError(status));
  return SIZE_MAX;
}

//===-- Futex functions ---------------------------------------------------===//

bool ts3_os_futex_wait(const _Atomic(uint32_t) *addr, uint32_t old,
                       uint32_t timeout_ms) {
  do {
    if (!WaitOnAddress((volatile void *)addr, &old, sizeof *addr, timeout_ms) &&
        GetLastError() == ERROR_TIMEOUT) {
      return false;
    }
  } while (atomic_load_explicit(addr, memory_order_relaxed) == old);
  return true;
}

void ts3_os_futex_wake(_Atomic(uint32_t) *addr) { WakeByAddressSingle(addr); }
