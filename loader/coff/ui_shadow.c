/*
 * SPDX-License-Identifier: MIT
 *
 * Hide Wallpaper Engine UI's glow/shadow windows (class "WPEUIShadow").
 *
 * wallpaperui.exe draws its window glow with four thin layered popups around
 * the main window. Under Wine's X11 driver those popups become
 * override-redirect X windows, which the X server/KWin always stack above
 * every managed window, so the glow is painted on top of whatever window
 * covers the WE UI. They are purely decorative, so keep them hidden.
 */

#include "ui_shadow.h"

#include "pe_iat.h"
#include "util.h"

typedef HWND(WINAPI *CreateWindowExW_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                        int, int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID);
typedef BOOL(WINAPI *ShowWindow_t)(HWND, int);
typedef BOOL(WINAPI *SetWindowPos_t)(HWND, HWND, int, int, int, int, UINT);
typedef HANDLE(WINAPI *DeferWindowPos_t)(HANDLE, HWND, HWND, int, int, int,
                                         int, UINT);

static CreateWindowExW_t real_CreateWindowExW;
static ShowWindow_t real_ShowWindow;
static SetWindowPos_t real_SetWindowPos;
static DeferWindowPos_t real_DeferWindowPos;

static const WCHAR kShadowClass[] = L"WPEUIShadow";

static BOOL is_shadow_window(HWND hwnd) {
  WCHAR cls[64];
  if (!hwnd || GetClassNameW(hwnd, cls, 64) <= 0) {
    return FALSE;
  }
  return lstrcmpW(cls, kShadowClass) == 0;
}

static HWND WINAPI fake_CreateWindowExW(DWORD exStyle, LPCWSTR className,
                                        LPCWSTR windowName, DWORD style, int x,
                                        int y, int w, int h, HWND parent,
                                        HMENU menu, HINSTANCE hinst,
                                        LPVOID param) {
  if (className && !IS_INTRESOURCE(className) &&
      lstrcmpW(className, kShadowClass) == 0) {
    style &= ~WS_VISIBLE;
  }
  return real_CreateWindowExW
             ? real_CreateWindowExW(exStyle, className, windowName, style, x,
                                    y, w, h, parent, menu, hinst, param)
             : NULL;
}

static BOOL WINAPI fake_ShowWindow(HWND hwnd, int cmdShow) {
  if (cmdShow != SW_HIDE && is_shadow_window(hwnd)) {
    return FALSE;
  }
  return real_ShowWindow ? real_ShowWindow(hwnd, cmdShow) : FALSE;
}

static BOOL WINAPI fake_SetWindowPos(HWND hwnd, HWND insertAfter, int x, int y,
                                     int cx, int cy, UINT flags) {
  if ((flags & SWP_SHOWWINDOW) && is_shadow_window(hwnd)) {
    flags &= ~SWP_SHOWWINDOW;
  }
  return real_SetWindowPos
             ? real_SetWindowPos(hwnd, insertAfter, x, y, cx, cy, flags)
             : FALSE;
}

static HANDLE WINAPI fake_DeferWindowPos(HANDLE info, HWND hwnd,
                                         HWND insertAfter, int x, int y,
                                         int cx, int cy, UINT flags) {
  if ((flags & SWP_SHOWWINDOW) && is_shadow_window(hwnd)) {
    flags &= ~SWP_SHOWWINDOW;
  }
  return real_DeferWindowPos ? real_DeferWindowPos(info, hwnd, insertAfter, x,
                                                   y, cx, cy, flags)
                             : NULL;
}

static BOOL running_as_wallpaperui(void) {
  WCHAR path[MAX_PATH];
  DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) {
    return FALSE;
  }
  return wide_contains_ci(path, L"wallpaperui.exe");
}

void install_ui_shadow_hooks(void) {
  if (!running_as_wallpaperui()) {
    return;
  }

  FARPROC orig = NULL;
  int n = patch_iat_all_modules("USER32.dll", "CreateWindowExW",
                                (FARPROC)(void *)fake_CreateWindowExW, &orig);
  real_CreateWindowExW = (CreateWindowExW_t)(void *)orig;

  orig = NULL;
  n += patch_iat_all_modules("USER32.dll", "ShowWindow",
                             (FARPROC)(void *)fake_ShowWindow, &orig);
  real_ShowWindow = (ShowWindow_t)(void *)orig;

  orig = NULL;
  n += patch_iat_all_modules("USER32.dll", "SetWindowPos",
                             (FARPROC)(void *)fake_SetWindowPos, &orig);
  real_SetWindowPos = (SetWindowPos_t)(void *)orig;

  orig = NULL;
  n += patch_iat_all_modules("USER32.dll", "DeferWindowPos",
                             (FARPROC)(void *)fake_DeferWindowPos, &orig);
  real_DeferWindowPos = (DeferWindowPos_t)(void *)orig;

  char b[96];
  wsprintfA(b, "install_ui_shadow_hooks: patched %d imports", n);
  debug_log(b);
}
