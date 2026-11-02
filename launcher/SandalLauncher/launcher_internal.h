// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <string>

#include "launcher_sign_in.h"

namespace aoc::launcher {

inline const COLORREF kBg       = RGB( 18,  22,  33);
inline const COLORREF kCardBg   = RGB( 18,  22,  33);
inline const COLORREF kCardBdr  = RGB( 34,  44,  65);
inline const COLORREF kFieldBg  = RGB( 12,  15,  23);
inline const COLORREF kFieldBdr = RGB( 38,  48,  70);
inline const COLORREF kAccent   = RGB(225,  45,  45);
inline const COLORREF kAccentHv = RGB(245,  65,  65);
inline const COLORREF kAccentPs = RGB(185,  30,  30);
inline const COLORREF kAccentDs = RGB( 42,  30,  34);
inline const COLORREF kBtnDark  = RGB( 28,  36,  53);
inline const COLORREF kBtnDkHv  = RGB( 40,  50,  74);
inline const COLORREF kBtnDkPs  = RGB( 22,  28,  42);
inline const COLORREF kTxtWht   = RGB(248, 250, 254);
inline const COLORREF kTxtSub   = RGB(145, 155, 178);
inline const COLORREF kTxtDim   = RGB( 88,  98, 120);
inline const COLORREF kTxtOk    = RGB( 52, 199,  89);
inline const COLORREF kTxtErr   = RGB(255,  69,  58);
inline const COLORREF kTxtWarn  = RGB(255, 179,  64);

inline constexpr int kW = 450;

inline constexpr int kCardX  = 0;
inline constexpr int kCardW  = kW;
inline constexpr int kCard1Y = 0;
inline constexpr int kCard1H = 220;
inline constexpr int kCard2Y = 220;
inline constexpr int kCard2H = 82;
inline constexpr int kCard3Y = 302;
inline constexpr int kCard3H = 96;
inline constexpr int kH      = 398;

inline constexpr int kKeepY     = 136;
inline constexpr int kAuthBtnY  = 164;
inline constexpr int kFormFbY   = 200;
inline constexpr int kSessLineY = 56;
inline constexpr int kSessNoteY = 84;
inline constexpr int kSessBtnY  = 120;
inline constexpr int kSessFbY   = 166;

inline constexpr int kUserMaxChars = 64;

enum ControlId {
    ID_USER       = 101,
    ID_PASS       = 102,
    ID_LOGIN      = 103,
    ID_REG        = 104,
    ID_LAUNCH     = 105,
    ID_BROWSE     = 106,
    ID_AUTODETECT = 107,
    ID_LOGOUT     = 108,
    ID_KEEP       = 109,
    ID_CANCEL     = IDCANCEL,
};

inline constexpr UINT WM_AUTH_DONE   = WM_APP + 1;
inline constexpr UINT WM_AUTO_STATUS = WM_APP + 2;
inline constexpr UINT WM_AUTO_DONE   = WM_APP + 3;
inline constexpr UINT WM_GAME_EXITED = WM_APP + 4;

struct AuthResult {
    bool ok;
    std::string message;
    std::string username;
    std::string note;
};

struct AutoDone {
    AutoOutcome outcome;
    std::string message;
    std::wstring username;
};

extern HBRUSH g_brBg;
extern HBRUSH g_brCard;
extern HBRUSH g_brField;
extern HFONT  g_fntHeader;
extern HFONT  g_fntMed;
extern HFONT  g_fntBig;
extern HFONT  g_fntForm;
extern HFONT  g_fntLabel;
extern HFONT  g_fntSmall;

extern bool         g_loggedIn;
extern std::wstring g_username;
extern std::wstring g_feedback;
extern bool         g_feedbackErr;
extern std::string  g_gameExe;
extern bool         g_gameExeOk;

extern bool         g_screenshotMode;
extern std::wstring g_previewState;
extern bool         g_gameRunning;

extern bool g_hoverLogin;
extern bool g_hoverReg;
extern bool g_hoverLaunch;
extern bool g_hoverBrowse;
extern bool g_hoverAutoDetect;
extern bool g_hoverLogout;
extern bool g_hoverKeep;
extern bool g_hoverCancel;

extern bool              g_keepSignedIn;
extern bool              g_autoActive;
extern std::wstring      g_autoUser;
extern std::wstring      g_autoStatus;
extern std::atomic<bool> g_autoCancel;
extern std::atomic<bool> g_authBusy;

extern HWND g_hwnd;
extern HWND g_lastFocus;

std::wstring to_wide(const std::string& s);
std::string to_utf8(const wchar_t* w);
std::string exe_dir();
bool file_exists(const std::string& p);

std::string load_game_exe();
bool save_game_exe(const std::string& exe_path);
void set_game_exe(const std::string& path);
std::string auto_detect_game();
std::string browse_for_game(HWND hwnd, const std::string& current_path);

std::string start_game(HANDLE* process);
void watch_game(HWND hwnd, HANDLE process);
void auth_worker(HWND hwnd, std::wstring wuser, std::wstring wpass, bool is_reg, bool keep);
void begin_auto_sign_in(HWND hwnd, const SavedSignIn& saved);

void migrate_legacy_state_if_present();
std::wstring load_remembered_user();
void save_remembered_user(const std::string& name);

void render_launcher(HWND hwnd, HDC dc);
void on_paint(HWND hwnd);
void on_draw_item(DRAWITEMSTRUCT* dis);
bool capture_launcher_to_file(HWND hwnd, const std::wstring& path);
void sync_controls(HWND hwnd);
int default_button_id(HWND hwnd);
HWND default_focus(HWND hwnd);
void restore_focus(HWND hwnd);
void focus_control(HWND hwnd, HWND c);

} // namespace aoc::launcher
