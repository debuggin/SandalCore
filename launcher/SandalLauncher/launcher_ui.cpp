// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "launcher_internal.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace aoc::launcher {

struct TextStyle {
    HFONT font;
    COLORREF color;
    UINT flags;
};

struct BtnState {
    bool hover;
    bool pressed;
    bool enabled;
};

struct BtnStyle {
    HFONT font;
    COLORREF baseBg;
    COLORREF hvBg;
    COLORREF psBg;
    COLORREF dsBg;
    COLORREF txtCol = kTxtWht;
    COLORREF bdrCol = 0;
    bool hasBdr = false;
};

static RECT xywh(int x, int y, int w, int h)
{
    return RECT{ x, y, x + w, y + h };
}

static void fill_rect(HDC dc, RECT r, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

static void draw_text_ex(HDC dc, const wchar_t* txt, RECT r, TextStyle style)
{
    SelectObject(dc, style.font);
    SetTextColor(dc, style.color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, txt, -1, &r, style.flags);
}

static void draw_rounded_btn(HDC dc, RECT r, const wchar_t* txt, BtnState st, const BtnStyle& style)
{
    const COLORREF bg = !st.enabled ? style.dsBg : st.pressed ? style.psBg : st.hover ? style.hvBg : style.baseBg;
    const COLORREF penCol = (style.hasBdr && st.enabled) ? (st.hover ? kAccentHv : style.bdrCol) : bg;
    HBRUSH br = CreateSolidBrush(bg);
    HPEN pen  = CreatePen(PS_SOLID, 1, penCol);

    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN op   = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 6, 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);

    SelectObject(dc, style.font);
    SetTextColor(dc, st.enabled ? style.txtCol : kTxtDim);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void draw_focus_mark(HDC dc, RECT r, COLORREF col)
{
    InflateRect(&r, -3, -3);
    HPEN   pen = CreatePen(PS_SOLID, 1, col);
    HPEN   op  = (HPEN)SelectObject(dc, pen);
    HBRUSH ob  = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 4, 4);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

static void draw_field_outline(HDC dc, RECT r)
{
    HPEN pen = CreatePen(PS_SOLID, 1, kFieldBdr);
    HPEN op  = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left - 1, r.top - 1, r.right + 1, r.bottom + 1, 4, 4);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

static void render_account_section(HDC dc)
{
    const UINT centerEllipsis = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    if (g_autoActive) {
        draw_text_ex(dc, L"SIGNING IN", xywh(18, 14, 260, 20), {g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE});
        std::wstring sUser = L"Signing in as  " + g_autoUser + L"\u2026";
        draw_text_ex(dc, sUser.c_str(), xywh(18, kSessLineY, kW - 36, 26), {g_fntBig, kTxtWht, centerEllipsis});
        draw_text_ex(dc, g_autoStatus.c_str(), xywh(18, kSessNoteY, kW - 36, 18),
                     {g_fntSmall, kTxtSub, DT_CENTER | DT_VCENTER | DT_SINGLELINE});
    } else if (!g_loggedIn) {
        draw_text_ex(dc, L"ACCOUNT LOGIN", xywh(18, 14, 200, 20), {g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE});
        draw_text_ex(dc, L"USERNAME", xywh(18, 38, 120, 16), {g_fntLabel, kTxtSub, DT_LEFT | DT_SINGLELINE});
        draw_field_outline(dc, xywh(18, 54, kW - 36, 26));
        draw_text_ex(dc, L"PASSWORD", xywh(18, 86, 120, 16), {g_fntLabel, kTxtSub, DT_LEFT | DT_SINGLELINE});
        draw_field_outline(dc, xywh(18, 102, kW - 36, 26));
    } else {
        draw_text_ex(dc, L"ACCOUNT", xywh(18, 14, 260, 20), {g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE});
        std::wstring sUser = L"\u2713  Signed in as  " + g_username;
        draw_text_ex(dc, sUser.c_str(), xywh(18, kSessLineY + 12, kW - 36, 26), {g_fntBig, kTxtOk, centerEllipsis});
    }

    if (!g_feedback.empty()) {
        const int fbY = (g_loggedIn || g_autoActive) ? kSessFbY : kFormFbY;
        draw_text_ex(dc, g_feedback.c_str(), xywh(18, fbY, kW - 36, 18),
                     {g_fntSmall, g_feedbackErr ? kTxtErr : kTxtOk, centerEllipsis});
    }
    fill_rect(dc, xywh(0, kCard2Y - 1, kW, 1), kCardBdr);
}

static void render_client_section(HDC dc)
{
    const bool exeOk = g_gameExeOk;
    draw_text_ex(dc, L"GAME CLIENT", xywh(18, kCard2Y + 10, 100, 18), {g_fntHeader, kTxtWht, DT_LEFT | DT_SINGLELINE});
    if (exeOk) {
        draw_text_ex(dc, L"\u2713  READY", xywh(120, kCard2Y + 11, 100, 18), {g_fntLabel, kTxtOk, DT_LEFT | DT_SINGLELINE});
    } else {
        draw_text_ex(dc, L"\u26a0  NOT FOUND", xywh(120, kCard2Y + 11, 150, 18), {g_fntLabel, kTxtErr, DT_LEFT | DT_SINGLELINE});
    }

    const RECT boxRect = xywh(18, kCard2Y + 34, 230, 32);
    fill_rect(dc, boxRect, kFieldBg);
    draw_field_outline(dc, boxRect);

    std::wstring pathStr = exeOk ? to_wide(g_gameExe) : L"No game selected (use the buttons \u2192)";
    draw_text_ex(dc, pathStr.c_str(), xywh(26, kCard2Y + 38, 214, 24),
                 {g_fntSmall, exeOk ? kTxtSub : kTxtDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS});
    fill_rect(dc, xywh(0, kCard3Y - 1, kW, 1), kCardBdr);
}

static void render_launch_section(HDC dc)
{
    std::wstring launchHint;
    COLORREF hintCol = kTxtSub;
    if (!g_gameExeOk) {
        launchHint = L"\u26a0  The game was not found. Use Auto-Detect or Browse.";
        hintCol = kTxtErr;
    } else if (g_gameRunning) {
        launchHint = L"The game is running.";
        hintCol = kTxtSub;
    } else if (g_autoActive) {
        launchHint = L"Signing in with your saved sign-in\u2026";
        hintCol = kTxtSub;
    } else if (!g_loggedIn) {
        launchHint = L"Please sign in or register above to launch.";
        hintCol = kTxtWarn;
    } else {
        launchHint = L"\u2713  Ready to play.";
        hintCol = kTxtOk;
    }
    draw_text_ex(dc, launchHint.c_str(), xywh(18, kCard3Y + 68, kW - 36, 18),
                 {g_fntSmall, hintCol, DT_CENTER | DT_VCENTER | DT_SINGLELINE});
}

void render_launcher(HWND, HDC dc)
{
    fill_rect(dc, xywh(0, 0, kW, kH), kBg);
    render_account_section(dc);
    render_client_section(dc);
    render_launch_section(dc);
}

void on_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    render_launcher(hwnd, dc);
    EndPaint(hwnd, &ps);
}

static void draw_keep_checkbox(DRAWITEMSTRUCT* dis)
{
    HDC dc = dis->hDC;
    const RECT r = dis->rcItem;
    const bool enabled = (dis->itemState & ODS_DISABLED) == 0;
    fill_rect(dc, r, kBg);

    const int box = 14;
    const int bx = r.left + 6;
    const int by = r.top + ((r.bottom - r.top) - box) / 2;
    const COLORREF fill = g_keepSignedIn ? kAccent : kFieldBg;
    const COLORREF edge = g_keepSignedIn ? kAccent : (g_hoverKeep ? kAccentHv : kFieldBdr);
    HBRUSH br  = CreateSolidBrush(fill);
    HPEN   pen = CreatePen(PS_SOLID, 1, edge);
    HBRUSH ob  = (HBRUSH)SelectObject(dc, br);
    HPEN   op  = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, bx, by, bx + box, by + box, 4, 4);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);

    if (g_keepSignedIn) {
        HPEN tick = CreatePen(PS_SOLID, 2, kTxtWht);
        HPEN old  = (HPEN)SelectObject(dc, tick);
        MoveToEx(dc, bx + 3, by + 7, nullptr);
        LineTo(dc, bx + 6, by + 10);
        LineTo(dc, bx + 11, by + 4);
        SelectObject(dc, old);
        DeleteObject(tick);
    }

    const COLORREF textCol = !enabled ? kTxtDim : g_hoverKeep ? kTxtWht : kTxtSub;
    draw_text_ex(dc, L"Keep me signed in", RECT{bx + box + 8, r.top, r.right, r.bottom},
                 {g_fntSmall, textCol, DT_LEFT | DT_VCENTER | DT_SINGLELINE});
}

void on_draw_item(DRAWITEMSTRUCT* dis)
{
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool enabled = (dis->itemState & ODS_DISABLED) == 0;
    const RECT r = dis->rcItem;
    const BtnStyle darkBtn{g_fntMed, kBtnDark, kBtnDkHv, kBtnDkPs, kBtnDark, kTxtWht, kCardBdr, true};
    const BtnStyle accentMed{g_fntMed, kAccent, kAccentHv, kAccentPs, kAccentDs};
    const BtnStyle accentBig{g_fntBig, kAccent, kAccentHv, kAccentPs, kAccentDs};

    switch (dis->CtlID) {
    case ID_BROWSE:
        draw_rounded_btn(dis->hDC, r, L"Browse\u2026", {g_hoverBrowse, pressed, enabled}, darkBtn);
        break;
    case ID_AUTODETECT:
        draw_rounded_btn(dis->hDC, r, L"Auto-Detect", {g_hoverAutoDetect, pressed, enabled}, darkBtn);
        break;
    case ID_LOGIN:
        draw_rounded_btn(dis->hDC, r, L"Login", {g_hoverLogin, pressed, enabled}, accentMed);
        break;
    case ID_REG:
        draw_rounded_btn(dis->hDC, r, L"Register", {g_hoverReg, pressed, enabled}, darkBtn);
        break;
    case ID_LOGOUT:
        draw_rounded_btn(dis->hDC, r, L"Switch Account / Logout", {g_hoverLogout, pressed, enabled}, darkBtn);
        break;
    case ID_CANCEL:
        draw_rounded_btn(dis->hDC, r, L"Cancel", {g_hoverCancel, pressed, enabled}, darkBtn);
        break;
    case ID_KEEP:
        draw_keep_checkbox(dis);
        break;
    case ID_LAUNCH:
        draw_rounded_btn(dis->hDC, r, g_gameRunning ? L"GAME RUNNING" : L"LAUNCH GAME",
                         {g_hoverLaunch, pressed, enabled}, accentBig);
        break;
    }

    if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT)) {
        draw_focus_mark(dis->hDC, r, (dis->CtlID == ID_LOGIN || dis->CtlID == ID_LAUNCH) ? kTxtWht : kAccentHv);
    }
}

static bool save_bitmap_to_file(HBITMAP hbm, int w, int h, const std::wstring& path)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    const DWORD rowSize = ((w * 3 + 3) & ~3);
    const DWORD imageSize = rowSize * h;
    std::vector<BYTE> pixels(imageSize, 0);

    HDC screenDC = GetDC(nullptr);
    int lines = GetDIBits(screenDC, hbm, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screenDC);
    if (lines <= 0) return false;

    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + imageSize;

    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    f.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof(bi.bmiHeader));
    f.write(reinterpret_cast<const char*>(pixels.data()), imageSize);
    return true;
}

static void render_child_control_to_dc(HWND hwnd, HWND c, HDC memDC)
{
    RECT cr;
    GetWindowRect(c, &cr);
    POINT pt1{ cr.left, cr.top }, pt2{ cr.right, cr.bottom };
    ScreenToClient(hwnd, &pt1);
    ScreenToClient(hwnd, &pt2);
    RECT r{ pt1.x, pt1.y, pt2.x, pt2.y };

    const int id = GetDlgCtrlID(c);
    const LONG style = GetWindowLongW(c, GWL_STYLE);
    if ((style & BS_OWNERDRAW) == BS_OWNERDRAW) {
        DRAWITEMSTRUCT dis{};
        dis.CtlType = ODT_BUTTON;
        dis.CtlID = id;
        dis.hwndItem = c;
        dis.hDC = memDC;
        dis.rcItem = r;
        dis.itemState = IsWindowEnabled(c) ? 0 : ODS_DISABLED;
        on_draw_item(&dis);
        return;
    }

    fill_rect(memDC, r, kFieldBg);
    draw_field_outline(memDC, r);
    wchar_t buf[256]{};
    GetWindowTextW(c, buf, 256);
    if ((style & ES_PASSWORD) != 0) {
        // Mask typed password characters in headless screenshots so credentials never leak into BMP captures.
        for (wchar_t* p = buf; *p; ++p) *p = L'\u2022';
    }
    if (buf[0] != L'\0') {
        draw_text_ex(memDC, buf, RECT{r.left + 6, r.top + 4, r.right - 6, r.bottom - 4},
                     {g_fntForm, kTxtWht, DT_LEFT | DT_VCENTER | DT_SINGLELINE});
    }
}

bool capture_launcher_to_file(HWND hwnd, const std::wstring& path)
{
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hbm = CreateCompatibleBitmap(screenDC, kW, kH);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hbm);

    render_launcher(hwnd, memDC);

    HWND c = GetWindow(hwnd, GW_CHILD);
    while (c) {
        if (IsWindowVisible(c)) render_child_control_to_dc(hwnd, c, memDC);
        c = GetWindow(c, GW_HWNDNEXT);
    }

    SelectObject(memDC, oldBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    bool ok = save_bitmap_to_file(hbm, kW, kH, path);
    DeleteObject(hbm);
    return ok;
}

void sync_controls(HWND hwnd)
{
    const bool lo = g_loggedIn;
    const bool form = !lo && !g_autoActive;

    ShowWindow(GetDlgItem(hwnd, ID_USER),   form ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_PASS),   form ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_KEEP),   form ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_LOGIN),  form ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_REG),    form ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_LOGOUT), lo ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_CANCEL), g_autoActive ? SW_SHOW : SW_HIDE);

    EnableWindow(GetDlgItem(hwnd, ID_LAUNCH), lo && g_gameExeOk && !g_gameRunning);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static bool is_interactive_button_id(int fid)
{
    switch (fid) {
    case ID_LOGIN:
    case ID_REG:
    case ID_LAUNCH:
    case ID_BROWSE:
    case ID_AUTODETECT:
    case ID_LOGOUT:
    case ID_CANCEL:
        return true;
    default:
        return false;
    }
}

int default_button_id(HWND hwnd)
{
    int id = g_autoActive ? 0 : g_loggedIn ? ID_LAUNCH : ID_LOGIN;
    const HWND f = GetFocus();
    if (f && IsChild(hwnd, f)) {
        const int fid = GetDlgCtrlID(f);
        if (is_interactive_button_id(fid)) id = fid;
    }
    const HWND b = id ? GetDlgItem(hwnd, id) : nullptr;
    return (b && IsWindowVisible(b) && IsWindowEnabled(b)) ? id : 0;
}

HWND default_focus(HWND hwnd)
{
    if (g_autoActive) return nullptr;
    if (g_loggedIn) {
        const HWND l = GetDlgItem(hwnd, ID_LAUNCH);
        return (IsWindowVisible(l) && IsWindowEnabled(l)) ? l : nullptr;
    }
    const HWND u = GetDlgItem(hwnd, ID_USER);
    return GetWindowTextLengthW(u) > 0 ? GetDlgItem(hwnd, ID_PASS) : u;
}

void restore_focus(HWND hwnd)
{
    HWND t = g_lastFocus;
    if (!t || !IsChild(hwnd, t) || !IsWindowVisible(t) || !IsWindowEnabled(t))
        t = default_focus(hwnd);
    if (t) SetFocus(t);
}

void focus_control(HWND hwnd, HWND c)
{
    if (!c) return;
    if (GetActiveWindow() == hwnd) SetFocus(c);
    else g_lastFocus = c;
}

} // namespace aoc::launcher
