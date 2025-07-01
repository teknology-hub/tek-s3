//===-- main_windows.c - program entry point for Windows ------------------===//
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
/// Service skeleton for Windows systems.
///
//===----------------------------------------------------------------------===//

#include "impl.h"
#include "os.h"

#include <locale.h>
#include <process.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <winsvc.h>

/// Futex value that is set to `1` after the program has stopped and cleaned up.
static _Atomic(uint32_t) ts3_done;

/// Console control signal handler.
///
/// @param type
///    Control signal type
/// @return `TRUE`.
static BOOL ts3_console_ctrl_handler(DWORD type) {
  switch (type) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
    ts3_stop();
    break;
  default:
    ts3_stop();
    ts3_os_futex_wait(&ts3_done, 0, 120000);
  }
  return TRUE;
}

/// Window procedure for handling OS shutdown events.
static LRESULT CALLBACK ts3_wnd_proc(HWND hWnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {
  switch (msg) {
  case WM_QUERYENDSESSION:
    ts3_stop();
    return TRUE;
  case WM_ENDSESSION:
    ts3_os_futex_wait(&ts3_done, 0, 120000);
    return 0;
  default:
    return DefWindowProcW(hWnd, msg, wParam, lParam);
  }
}

/// Thread procedure that creates a hidden window and processes its messages in
///    a loop.
///
/// @return `0`.
static unsigned ts3_wnd_thrd_proc(void *) {
  auto const class =
      RegisterClassExW(&(const WNDCLASSEXW){.cbSize = sizeof(WNDCLASSEXW),
                                            .lpfnWndProc = ts3_wnd_proc,
                                            .lpszClassName = L"TEK_S3"});
  if (!class) {
    fputs("Warning: Failed to register window class\n", stderr);
    return 0;
  }
  auto const hwnd = CreateWindowExW(
      0, MAKEINTATOM(class), nullptr, 0, CW_USEDEFAULT, CW_USEDEFAULT,
      CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, nullptr, nullptr);
  if (!hwnd) {
    fputs("Warning: Failed to create window instance\n", stderr);
    return 0;
  }
  ShowWindow(hwnd, SW_HIDE);
  for (MSG msg; GetMessageW(&msg, nullptr, 0, 0);) {
    DispatchMessageW(&msg);
  }
  return 0;
}

/// Register tek-s3 as a Windows service.
static int ts3_reg_svc(void) {
  WCHAR path[PATH_MAX];
  if (!GetModuleFileNameW(nullptr, path, sizeof path / sizeof *path)) {
    fputs("Error: Failed to get module file path\n", stderr);
    return EXIT_FAILURE;
  }
  // Check if current process is elevated
  TOKEN_ELEVATION elevation;
  DWORD ret_size;
  if (!GetTokenInformation(GetCurrentProcessToken(), TokenElevation, &elevation,
                           sizeof elevation, &ret_size)) {
    fputs("Error: Failed to get process token information\n", stderr);
    return EXIT_FAILURE;
  }
  if (!elevation.TokenIsElevated) {
    if (!ShellExecuteExW(&(SHELLEXECUTEINFOW){
            .cbSize = sizeof(SHELLEXECUTEINFOW),
            .fMask = SEE_MASK_NOASYNC | SEE_MASK_UNICODE | SEE_MASK_NO_CONSOLE,
            .lpVerb = L"runas",
            .lpFile = path,
            .lpParameters = L"--register-svc",
            .nShow = SW_NORMAL})) {
      fputs("Error: Failed to start elevated process\n", stderr);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }
  auto const scm = OpenSCManagerW(
      nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
  if (!scm) {
    fputs("Error: Failed to open SCM connection\n", stderr);
    return EXIT_FAILURE;
  }
  WCHAR svc_cmd[sizeof path / sizeof *path + sizeof "\"--run-svc\""];
  swprintf_s(svc_cmd, sizeof svc_cmd / sizeof *svc_cmd, L"\"%s\" --run-svc",
             path);
  auto const svc = CreateServiceW(
      scm, L"tek-s3", L"TEK Steam Sharing Server", 0, SERVICE_WIN32_OWN_PROCESS,
      SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, svc_cmd, nullptr, nullptr,
      nullptr, L"NT SERVICE\\tek-s3", nullptr);
  if (!svc) {
    CloseServiceHandle(scm);
    fputs("Error: Failed to create the service\n", stderr);
    return EXIT_FAILURE;
  }
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return EXIT_SUCCESS;
}

/// Global storage for service status handle.
static SERVICE_STATUS_HANDLE _Nullable ts3_status_handle;

/// Service control handler function.
///
/// @param control
///    Service control code.
static DWORD WINAPI ts3_svc_handler(DWORD control, DWORD, LPVOID, LPVOID) {
  switch (control) {
  case SERVICE_CONTROL_STOP:
  case SERVICE_CONTROL_PRESHUTDOWN:
    SetServiceStatus(
        ts3_status_handle,
        &(SERVICE_STATUS){.dwServiceType = SERVICE_WIN32_OWN_PROCESS,
                          .dwCurrentState = SERVICE_STOP_PENDING,
                          .dwWaitHint = 15000});
    ts3_stop();
    return NO_ERROR;
  case SERVICE_CONTROL_INTERROGATE:
    return NO_ERROR;
  default:
    return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

/// Service main function.
static VOID WINAPI ts3_svc_main(DWORD, LPWSTR *) {
  ts3_status_handle =
      RegisterServiceCtrlHandlerExW(L"tek-s3", ts3_svc_handler, nullptr);
  if (!ts3_status_handle) {
    return;
  }
  SERVICE_STATUS status = {.dwServiceType = SERVICE_WIN32_OWN_PROCESS,
                           .dwCurrentState = SERVICE_START_PENDING,
                           .dwWaitHint = 1000};
  SetServiceStatus(ts3_status_handle, &status);
  status.dwWaitHint = 0;
  if (!ts3_init()) {
    status.dwCurrentState = SERVICE_STOPPED;
    status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    status.dwServiceSpecificExitCode = 1;
    SetServiceStatus(ts3_status_handle, &status);
    return;
  }
  status.dwCurrentState = SERVICE_RUNNING;
  status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN;
  SetServiceStatus(ts3_status_handle, &status);
  status.dwControlsAccepted = 0;
  ts3_run();
  status.dwCurrentState = SERVICE_STOP_PENDING;
  status.dwCheckPoint = 1;
  status.dwWaitHint = 5000;
  SetServiceStatus(ts3_status_handle, &status);
  const int res = ts3_cleanup();
  status.dwCurrentState = SERVICE_STOPPED;
  if (res != EXIT_SUCCESS) {
    status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    status.dwServiceSpecificExitCode = res;
  }
  status.dwCheckPoint = 0;
  status.dwWaitHint = 0;
  SetServiceStatus(ts3_status_handle, &status);
}

int wmain(int argc, wchar_t *argv[]) {
  setlocale(LC_ALL, ".UTF-8");
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
  if (argc > 1) {
    if (!wcscmp(argv[1], L"--register-svc")) {
      return ts3_reg_svc();
    } else if (!wcscmp(argv[1], L"--run-svc")) {
      return StartServiceCtrlDispatcherW(&(const SERVICE_TABLE_ENTRYW){
                 .lpServiceName = L"tek-s3", .lpServiceProc = ts3_svc_main})
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;
    }
  }
  if (!ts3_init()) {
    return EXIT_FAILURE;
  }
  SetConsoleCtrlHandler(ts3_console_ctrl_handler, TRUE);
  _beginthreadex(nullptr, 0, ts3_wnd_thrd_proc, nullptr, 0, nullptr);
  ts3_run();
  const int res = ts3_cleanup();
  atomic_store_explicit(&ts3_done, 1, memory_order_relaxed);
  ts3_os_futex_wake(&ts3_done);
  return res;
}
