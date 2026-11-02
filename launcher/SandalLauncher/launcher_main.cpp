// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "launcher_internal.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <memory>
#include <thread>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace aoc::launcher {

HBRUSH g_brBg      = nullptr;
HBRUSH g_brCard    = nullptr;
HBRUSH g_brField   = nullptr;
HFONT  g_fntHeader = nullptr;
HFONT  g_fntMed    = nullptr;
HFONT  g_fntBig    = nullptr;
HFONT  g_fntForm   = nullptr;
HFONT  g_fntLabel  = nullptr;
HFONT  g_fntSmall  = nullptr;

bool         g_loggedIn    = false;
std::wstring g_username;
std::wstring g_feedback;
bool         g_feedbackErr = false;
std::string  g_gameExe;
bool         g_gameExeOk   = false;

bool         g_screenshotMode = false;
std::wstring g_previewState;
bool         g_gameRunning    = false;

bool g_hoverLogin      = false;
bool g_hoverReg        = false;
bool g_hoverLaunch     = false;
bool g_hoverBrowse     = false;
bool g_hoverAutoDetect = false;
bool g_hoverLogout     = false;
bool g_hoverKeep       = false;
bool g_hoverCancel     = false;

bool              g_keepSignedIn = false;
bool              g_autoActive   = false;
std::wstring      g_autoUser;
std::wstring      g_autoStatus;
std::atomic<bool> g_autoCancel{false};
std::atomic<bool> g_authBusy{false};

HWND g_hwnd      = nullptr;
HWND g_lastFocus = nullptr;

static void create_launcher_controls(HWND hwnd, HINSTANCE hi)
{
    HWND hU = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        18, 54, kW - 36, 26, hwnd, (HMENU)(INT_PTR)ID_USER, hi, nullptr);
    SendMessageW(hU, EM_SETLIMITTEXT, kUserMaxChars, 0);

    HWND hP = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
        18, 102, kW - 36, 26, hwnd, (HMENU)(INT_PTR)ID_PASS, hi, nullptr);
    SendMessageW(hP, EM_SETLIMITTEXT, 128, 0);

    CreateWindowW(L"BUTTON", L"Keep me signed in",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        18, kKeepY - 2, 190, 24, hwnd, (HMENU)(INT_PTR)ID_KEEP, hi, nullptr);

    const int authBtnW = (kW - 36 - 10) / 2;
    CreateWindowW(L"BUTTON", L"Login",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON,
        18, kAuthBtnY, authBtnW, 34, hwnd, (HMENU)(INT_PTR)ID_LOGIN, hi, nullptr);
    CreateWindowW(L"BUTTON", L"Register",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        18 + authBtnW + 10, kAuthBtnY, authBtnW, 34, hwnd, (HMENU)(INT_PTR)ID_REG, hi, nullptr);
    CreateWindowW(L"BUTTON", L"Switch Account / Logout",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        (kW - 210) / 2, kSessBtnY, 210, 34, hwnd, (HMENU)(INT_PTR)ID_LOGOUT, hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        (kW - 210) / 2, kSessBtnY, 210, 34, hwnd, (HMENU)(INT_PTR)ID_CANCEL, hi, nullptr);
    CreateWindowW(L"BUTTON", L"Browse\u2026",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        256, kCard2Y + 34, 84, 32, hwnd, (HMENU)(INT_PTR)ID_BROWSE, hi, nullptr);
    CreateWindowW(L"BUTTON", L"Auto-Detect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        348, kCard2Y + 34, 84, 32, hwnd, (HMENU)(INT_PTR)ID_AUTODETECT, hi, nullptr);
    CreateWindowW(L"BUTTON", L"LAUNCH GAME",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED,
        18, kCard3Y + 14, kW - 36, 46, hwnd, (HMENU)(INT_PTR)ID_LAUNCH, hi, nullptr);

    SendMessageW(hU, WM_SETFONT, (WPARAM)g_fntForm, TRUE);
    SendMessageW(hP, WM_SETFONT, (WPARAM)g_fntForm, TRUE);
}

static void apply_preview_state()
{
    if (g_previewState == L"signed-in") {
        g_loggedIn = true;
        g_username = L"Example";
    } else if (g_previewState == L"signing-in") {
        g_autoActive = true;
        g_autoUser = L"Example";
        g_autoStatus = L"Starting in 3 s. Press Cancel to use another account.";
    } else if (g_previewState == L"form-kept") {
        g_keepSignedIn = true;
    } else if (g_previewState == L"game-running") {
        g_loggedIn = true;
        g_username = L"Example";
        g_gameRunning = true;
    }
}

static void init_launcher_session(HWND hwnd)
{
    set_game_exe(load_game_exe());
    if (!g_screenshotMode && !g_gameExeOk) {
        const std::string detected = auto_detect_game();
        if (!detected.empty()) {
            set_game_exe(detected);
            save_game_exe(detected);
            g_feedback = L"Found the game.";
            g_feedbackErr = false;
        }
    }

    if (g_screenshotMode) {
        apply_preview_state();
        return;
    }

    migrate_legacy_state_if_present();
    std::wstring remembered = load_remembered_user();
    const auto saved = credential_load(kCredentialTarget);
    if (saved) {
        remembered = saved->username;
        g_keepSignedIn = true;
    }
    if (!remembered.empty()) SetDlgItemTextW(hwnd, ID_USER, remembered.c_str());
    if (saved) begin_auto_sign_in(hwnd, *saved);
}

static void on_wm_mousemove(HWND hwnd, LPARAM lp)
{
    const int mx = LOWORD(lp);
    const int my = (SHORT)HIWORD(lp);
    HWND hit = ChildWindowFromPointEx(hwnd, {mx, my}, CWP_SKIPINVISIBLE);

    auto refresh_btn = [](HWND h, bool& flag, bool want) {
        if (flag != want) {
            flag = want;
            if (h) InvalidateRect(h, nullptr, FALSE);
        }
    };
    refresh_btn(GetDlgItem(hwnd, ID_BROWSE),     g_hoverBrowse,     hit == GetDlgItem(hwnd, ID_BROWSE));
    refresh_btn(GetDlgItem(hwnd, ID_AUTODETECT), g_hoverAutoDetect, hit == GetDlgItem(hwnd, ID_AUTODETECT));
    refresh_btn(GetDlgItem(hwnd, ID_LOGIN),      g_hoverLogin,      hit == GetDlgItem(hwnd, ID_LOGIN));
    refresh_btn(GetDlgItem(hwnd, ID_REG),        g_hoverReg,        hit == GetDlgItem(hwnd, ID_REG));
    refresh_btn(GetDlgItem(hwnd, ID_LOGOUT),     g_hoverLogout,     hit == GetDlgItem(hwnd, ID_LOGOUT));
    refresh_btn(GetDlgItem(hwnd, ID_LAUNCH),     g_hoverLaunch,     hit == GetDlgItem(hwnd, ID_LAUNCH));
    refresh_btn(GetDlgItem(hwnd, ID_KEEP),       g_hoverKeep,       hit == GetDlgItem(hwnd, ID_KEEP));
    refresh_btn(GetDlgItem(hwnd, ID_CANCEL),     g_hoverCancel,     hit == GetDlgItem(hwnd, ID_CANCEL));
}

static void on_wm_auth_done(HWND hwnd, AuthResult* raw)
{
    std::unique_ptr<AuthResult> r(raw);
    const bool ok = r->ok;
    if (ok) {
        g_loggedIn = true;
        g_username = to_wide(r->username);
        g_feedback = to_wide(r->note);
        g_feedbackErr = !r->note.empty();
        save_remembered_user(r->username);
    } else {
        g_feedbackErr = true;
        g_feedback = to_wide(r->message);
    }
    sync_controls(hwnd);
    if (ok) focus_control(hwnd, default_focus(hwnd));
}

static void on_wm_auto_done(HWND hwnd, AutoDone* raw)
{
    std::unique_ptr<AutoDone> d(raw);
    if (!g_autoActive && d->outcome != AutoOutcome::SignedIn) return;
    g_autoActive = false;
    g_feedback.clear();
    g_feedbackErr = false;
    switch (d->outcome) {
    case AutoOutcome::SignedIn:
        g_loggedIn = true;
        g_username = d->username;
        save_remembered_user(to_utf8(d->username.c_str()));
        break;
    case AutoOutcome::Refused:
        g_feedback = L"Saved sign-in refused: " + to_wide(d->message) + L" Sign in again.";
        g_feedbackErr = true;
        break;
    case AutoOutcome::GaveUp:
        g_feedback = to_wide(d->message) + L" Try again when the server is running.";
        g_feedbackErr = true;
        break;
    case AutoOutcome::TokenNotSaved:
        g_feedback = to_wide(d->message);
        g_feedbackErr = true;
        break;
    case AutoOutcome::Cancelled:
        g_feedback = L"Automatic sign-in cancelled.";
        break;
    }
    sync_controls(hwnd);
    focus_control(hwnd, default_focus(hwnd));
}

static void handle_logout_command(HWND hwnd)
{
    const bool wasKept = g_keepSignedIn;
    const bool gone = credential_remove(kCredentialTarget);
    g_keepSignedIn = false;
    g_loggedIn = false;
    g_username.clear();
    g_feedback = !gone   ? L"Logged out, but Windows would not delete the saved sign-in."
               : wasKept ? L"Logged out. The saved sign-in is forgotten."
                         : L"Logged out.";
    g_feedbackErr = !gone;
    SetDlgItemTextW(hwnd, ID_PASS, L"");
    sync_controls(hwnd);
    focus_control(hwnd, GetDlgItem(hwnd, ID_USER));
}

static void handle_auth_command(HWND hwnd, int id)
{
    if (g_authBusy.exchange(true)) return;
    wchar_t wu[128]{}, wp2[128]{};
    GetDlgItemTextW(hwnd, ID_USER, wu, 128);
    GetDlgItemTextW(hwnd, ID_PASS, wp2, 128);
    std::wstring user = wu;
    std::wstring pass = wp2;
    SecureZeroMemory(wp2, sizeof(wp2));

    if (user.empty() || pass.empty()) {
        wipe(pass);
        g_feedback = L"Username and password are required.";
        g_feedbackErr = true;
        g_authBusy.store(false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    g_feedback = L"Signing in\u2026";
    g_feedbackErr = false;
    InvalidateRect(hwnd, nullptr, FALSE);

    const bool is_reg = (id == ID_REG);
    const bool keep = g_keepSignedIn;
    std::thread([hwnd, user, pass, is_reg, keep]() mutable {
        auth_worker(hwnd, user, pass, is_reg, keep);
        wipe(pass);
    }).detach();
    wipe(pass);
}

static void handle_launch_command(HWND hwnd)
{
    if (!g_loggedIn) return;
    if (g_gameRunning || game_is_running()) {
        g_feedback = L"The game is already running.";
        g_feedbackErr = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    g_feedback = L"Starting the game\u2026";
    g_feedbackErr = false;
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);

    HANDLE process = nullptr;
    const std::string err = start_game(&process);
    if (err.empty()) {
        g_gameRunning = true;
        g_feedback.clear();
        watch_game(hwnd, process);
    } else {
        g_feedback = to_wide(err);
        g_feedbackErr = true;
        MessageBoxA(hwnd, err.c_str(), "SandalCore", MB_OK | MB_ICONERROR);
    }
    sync_controls(hwnd);
}

static void handle_browse_or_detect(HWND hwnd, int id)
{
    if (id == ID_BROWSE) {
        std::string picked = browse_for_game(hwnd, g_gameExe);
        if (!picked.empty()) {
            set_game_exe(picked);
            save_game_exe(picked);
            g_feedback = L"Game location updated.";
            g_feedbackErr = false;
            sync_controls(hwnd);
        }
        return;
    }
    std::string found = auto_detect_game();
    if (!found.empty()) {
        set_game_exe(found);
        save_game_exe(found);
        g_feedback = L"Found the game on Steam.";
        g_feedbackErr = false;
    } else {
        g_feedback = L"Couldn't find the game. Please use Browse.";
        g_feedbackErr = true;
    }
    sync_controls(hwnd);
}

static void on_wm_command(HWND hwnd, WPARAM wp)
{
    int id = LOWORD(wp);
    if (id == IDOK) id = default_button_id(hwnd);

    if (id == ID_KEEP) {
        g_keepSignedIn = !g_keepSignedIn;
        InvalidateRect(GetDlgItem(hwnd, ID_KEEP), nullptr, FALSE);
    } else if (id == ID_CANCEL) {
        if (g_autoActive) {
            g_autoCancel.store(true);
            g_autoActive = false;
            g_feedback = L"Automatic sign-in cancelled.";
            g_feedbackErr = false;
            sync_controls(hwnd);
            focus_control(hwnd, default_focus(hwnd));
        }
    } else if (id == ID_BROWSE || id == ID_AUTODETECT) {
        handle_browse_or_detect(hwnd, id);
    } else if (id == ID_LOGOUT) {
        handle_logout_command(hwnd);
    } else if (id == ID_LOGIN || id == ID_REG) {
        handle_auth_command(hwnd, id);
    } else if (id == ID_LAUNCH) {
        handle_launch_command(hwnd);
    }
}

static bool handle_paint_or_color_msg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out)
{
    switch (msg) {
    case WM_PAINT:
        on_paint(hwnd);
        *out = 0;
        return true;
    case WM_PRINT:
    case WM_PRINTCLIENT:
        render_launcher(hwnd, (HDC)wp);
        *out = 0;
        return true;
    case WM_ERASEBKGND:
        *out = 1;
        return true;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, kTxtWht);
        SetBkColor((HDC)wp, kFieldBg);
        *out = (LRESULT)g_brField;
        return true;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, kTxtSub);
        SetBkColor((HDC)wp, kCardBg);
        *out = (LRESULT)g_brCard;
        return true;
    case WM_CTLCOLORBTN:
        *out = (LRESULT)g_brBg;
        return true;
    case WM_DRAWITEM:
        on_draw_item((DRAWITEMSTRUCT*)lp);
        *out = TRUE;
        return true;
    default:
        return false;
    }
}

static bool handle_focus_or_mouse_msg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out)
{
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            const HWND f = GetFocus();
            g_lastFocus = (f && IsChild(hwnd, f)) ? f : nullptr;
            return false;
        }
        if (!IsIconic(hwnd)) {
            restore_focus(hwnd);
            *out = 0;
            return true;
        }
        return false;
    case WM_SETFOCUS:
        if (!IsIconic(hwnd)) restore_focus(hwnd);
        *out = 0;
        return true;
    case DM_GETDEFID: {
        const int id = default_button_id(hwnd);
        *out = id ? MAKELRESULT(id, DC_HASDEFID) : 0;
        return true;
    }
    case WM_MOUSEMOVE:
        on_wm_mousemove(hwnd, lp);
        *out = 0;
        return true;
    case WM_MOUSELEAVE:
        g_hoverBrowse = g_hoverAutoDetect = g_hoverLogin = g_hoverReg = false;
        g_hoverLogout = g_hoverLaunch = g_hoverKeep = g_hoverCancel = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        *out = 0;
        return true;
    default:
        return false;
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT handledResult = 0;
    if (handle_paint_or_color_msg(hwnd, msg, wp, lp, &handledResult)) return handledResult;
    if (handle_focus_or_mouse_msg(hwnd, msg, wp, lp, &handledResult)) return handledResult;

    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        create_launcher_controls(hwnd, GetModuleHandleW(nullptr));
        init_launcher_session(hwnd);
        sync_controls(hwnd);
        return 0;
    case WM_AUTH_DONE:
        on_wm_auth_done(hwnd, (AuthResult*)lp);
        return 0;
    case WM_AUTO_STATUS: {
        std::unique_ptr<std::wstring> line((std::wstring*)lp);
        if (g_autoActive) {
            g_autoStatus = *line;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_AUTO_DONE:
        on_wm_auto_done(hwnd, (AutoDone*)lp);
        return 0;
    case WM_GAME_EXITED:
        g_gameRunning = false;
        g_feedback.clear();
        g_feedbackErr = false;
        sync_controls(hwnd);
        return 0;
    case WM_COMMAND:
        on_wm_command(hwnd, wp);
        return 0;
    case WM_DESTROY:
        // Cancel any in-flight automatic sign-in so it cannot write a token after window teardown.
        g_autoCancel.store(true);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static std::wstring parse_command_line()
{
    std::wstring screenshot;
    int n = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!argv) return screenshot;
    for (int i = 1; i < n; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--screenshot" && i + 1 < n)         screenshot = argv[++i];
        else if (a == L"--preview-state" && i + 1 < n) g_previewState = argv[++i];
    }
    LocalFree(argv);
    g_screenshotMode = !screenshot.empty();
    return screenshot;
}

static HFONT create_ui_font(int height, int weight)
{
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void init_gdi_resources()
{
    g_brBg      = CreateSolidBrush(kBg);
    g_brCard    = CreateSolidBrush(kCardBg);
    g_brField   = CreateSolidBrush(kFieldBg);
    g_fntHeader = create_ui_font(14, FW_BOLD);
    g_fntMed    = create_ui_font(13, FW_SEMIBOLD);
    g_fntBig    = create_ui_font(16, FW_BOLD);
    g_fntForm   = create_ui_font(14, FW_NORMAL);
    g_fntLabel  = create_ui_font(11, FW_BOLD);
    g_fntSmall  = create_ui_font(11, FW_NORMAL);
}

static void destroy_gdi_resources()
{
    DeleteObject(g_fntHeader);
    DeleteObject(g_fntMed);
    DeleteObject(g_fntBig);
    DeleteObject(g_fntForm);
    DeleteObject(g_fntLabel);
    DeleteObject(g_fntSmall);
    DeleteObject(g_brBg);
    DeleteObject(g_brCard);
    DeleteObject(g_brField);
}

static int run_message_loop(HWND hwnd)
{
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        // Ignore auto-repeated Enter key-downs so holding Enter on Login cannot trigger Launch Game on completion.
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && (m.lParam & (1 << 30)))
            continue;
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return (int)m.wParam;
}

int run_launcher(HINSTANCE hInst)
{
    const std::wstring screenshotPath = parse_command_line();

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    init_gdi_resources();

    WNDCLASSW wc{};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"SandalCoreLauncher";
    wc.hbrBackground = g_brBg;
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon         = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr{ 0, 0, kW, kH };
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    const int ww = wr.right - wr.left;
    const int wh = wr.bottom - wr.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    const int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT,
        L"SandalCoreLauncher",
        L"SandalCore \u2014 Ashes of Creation Emulator",
        style | WS_VISIBLE,
        sx, sy, ww, wh, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        destroy_gdi_resources();
        return 1;
    }

    int exitCode = 0;
    if (g_screenshotMode) {
        exitCode = capture_launcher_to_file(hwnd, screenshotPath) ? 0 : 2;
        DestroyWindow(hwnd);
    } else {
        exitCode = run_message_loop(hwnd);
    }

    destroy_gdi_resources();
    return exitCode;
}

} // namespace aoc::launcher

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    return aoc::launcher::run_launcher(hInst);
}
