// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "launcher_internal.h"

#include <commdlg.h>
#include <wininet.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include "services/tether/ics_token_file.h"

#if __has_include("launcher_legacy_names.h")
#  include "launcher_legacy_names.h"
#endif

namespace aoc::launcher {

using json = nlohmann::json;

constexpr DWORD kHttpDeadlineMs = 15000;
constexpr DWORD kHttpWaitMs     = 10000;

std::wstring to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

std::string to_utf8(const wchar_t* w)
{
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::string exe_dir()
{
    std::wstring w = wide_path_from(
        [](wchar_t* buf, DWORD size) { return ::GetModuleFileNameW(nullptr, buf, size); });
    const auto p = w.find_last_of(L"\\/");
    if (p == std::wstring::npos) return "";
    w.resize(p + 1);
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string json_str(const std::string& path, const std::string& key, const std::string& def = "")
{
    try {
        std::ifstream f(path);
        if (!f) return def;
        return json::parse(f).value(key, def);
    } catch (...) {
        return def;
    }
}

bool file_exists(const std::string& p)
{
    if (p.empty()) return false;
    std::error_code ec;
    const bool e = std::filesystem::exists(std::filesystem::path(p), ec);
    return e && !ec;
}

static std::vector<std::string> config_paths_candidates()
{
    std::vector<std::string> paths;
    const std::string dir = exe_dir();
    paths.push_back(dir + "config\\paths.json");
    paths.push_back(dir + "..\\..\\config\\paths.json");
    paths.push_back(dir + "..\\config\\paths.json");
    return paths;
}

std::string load_game_exe()
{
    for (const auto& p : config_paths_candidates()) {
        if (file_exists(p)) {
            std::string exe = json_str(p, "game_executable", "");
            if (file_exists(exe)) return exe;
        }
    }
    for (const auto& p : config_paths_candidates()) {
        if (file_exists(p)) {
            std::string exe = json_str(p, "game_executable", "");
            if (!exe.empty()) return exe;
        }
    }
    return json_str(exe_dir() + "config\\paths.json", "game_executable", "");
}

bool save_game_exe(const std::string& exe_path)
{
    const std::string p = exe_dir() + "config\\paths.json";
    try {
        json j = json::object();
        {
            std::ifstream in(p);
            if (in) {
                try { j = json::parse(in); } catch (...) { j = json::object(); }
            }
        }
        if (!j.is_object()) j = json::object();
        if (j.contains("game_executable") && j["game_executable"].is_string() &&
            j["game_executable"].get<std::string>() == exe_path)
            return true;
        j["game_executable"] = exe_path;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
        std::ofstream out(p, std::ios::trunc);
        if (!out) return false;
        out << j.dump(4, ' ', false, json::error_handler_t::replace) << "\n";
        out.flush();
        return static_cast<bool>(out);
    } catch (...) {
        return false;
    }
}

void set_game_exe(const std::string& path)
{
    g_gameExe = path;
    g_gameExeOk = file_exists(path);
}

static std::string extract_vdf_library_path(const std::string& line)
{
    const size_t p = line.find("\"path\"");
    if (p == std::string::npos) return {};
    const size_t q1 = line.find('\"', p + 6);
    if (q1 == std::string::npos) return {};
    const size_t q2 = line.find('\"', q1 + 1);
    if (q2 == std::string::npos) return {};
    const std::string lib = line.substr(q1 + 1, q2 - q1 - 1);
    std::string cleanLib;
    for (size_t i = 0; i < lib.size(); ++i) {
        if (lib[i] == '\\' && i + 1 < lib.size() && lib[i + 1] == '\\') {
            cleanLib += '\\';
            ++i;
        } else {
            cleanLib += lib[i];
        }
    }
    return cleanLib;
}

static void append_steam_roots(std::vector<std::string>& searchRoots)
{
    auto queryRegString = [](HKEY root, const wchar_t* subkey, const wchar_t* valName) -> std::string {
        return to_utf8(registry_string(root, subkey, valName).c_str());
    };

    std::string steamPath = queryRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steamPath.empty())
        steamPath = queryRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    if (steamPath.empty())
        steamPath = queryRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steamPath.empty()) return;

    for (char& c : steamPath) {
        if (c == '/') c = '\\';
    }
    searchRoots.push_back(steamPath);

    std::ifstream vdf(steamPath + "\\steamapps\\libraryfolders.vdf");
    if (!vdf) return;
    std::string line;
    while (std::getline(vdf, line)) {
        std::string cleanLib = extract_vdf_library_path(line);
        if (!cleanLib.empty()) searchRoots.push_back(cleanLib);
    }
}

static void append_fixed_drive_roots(std::vector<std::string>& searchRoots)
{
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((drives & (1u << i)) == 0) continue;
        std::string d = std::string(1, static_cast<char>('A' + i)) + ":";
        if (GetDriveTypeA((d + "\\").c_str()) != DRIVE_FIXED) continue;
        searchRoots.push_back(d);
        searchRoots.push_back(d + "\\Program Files (x86)\\Steam");
        searchRoots.push_back(d + "\\Steam");
        searchRoots.push_back(d + "\\Games");
        searchRoots.push_back(d + "\\Ashes of Creation");
        searchRoots.push_back(d + "\\AshesOfCreation");
    }
}

static std::string probe_root_for_client(const std::string& root, const std::vector<std::string>& subPaths)
{
    for (const auto& sub : subPaths) {
        std::string full = root + sub;
        if (!file_exists(full)) continue;
        if (full.size() >= 13 && full.ends_with("AOCClient.exe")) {
            std::string ship = full.substr(0, full.size() - 13) + "AOCClient-Win64-Shipping.exe";
            if (file_exists(ship)) return ship;
        }
        return full;
    }
    return "";
}

std::string auto_detect_game()
{
    std::vector<std::string> searchRoots;
    append_steam_roots(searchRoots);
    append_fixed_drive_roots(searchRoots);

    std::vector<std::string> uniqueRoots;
    for (const auto& r : searchRoots) {
        if (!r.empty() && std::find(uniqueRoots.begin(), uniqueRoots.end(), r) == uniqueRoots.end()) {
            uniqueRoots.push_back(r);
        }
    }

    const std::vector<std::string> subPaths = {
        "\\steamapps\\common\\Ashes of Creation\\Game\\AOC\\Binaries\\Win64\\AOCClient-Win64-Shipping.exe",
        "\\steamapps\\common\\Ashes of Creation\\Game\\AOC\\Binaries\\Win64\\AOCClient.exe",
        "\\Game\\AOC\\Binaries\\Win64\\AOCClient-Win64-Shipping.exe",
        "\\Game\\AOC\\Binaries\\Win64\\AOCClient.exe",
        "\\AOC\\Binaries\\Win64\\AOCClient-Win64-Shipping.exe",
        "\\AOC\\Binaries\\Win64\\AOCClient.exe"
    };

    for (const auto& root : uniqueRoots) {
        std::string found = probe_root_for_client(root, subPaths);
        if (!found.empty()) return found;
    }
    return "";
}

static std::wstring resolve_initial_browse_dir(const std::string& current_path)
{
    if (current_path.empty()) return {};
    try {
        std::filesystem::path p(current_path);
        if (file_exists(p.parent_path().string())) return p.parent_path().wstring();
    } catch (...) {
        return {};
    }
    return {};
}

static std::string prefer_shipping_exe(const std::string& selected)
{
    try {
        std::filesystem::path sp(selected);
        if (sp.filename() == "AOCClient.exe") {
            auto ship = sp.parent_path() / "AOCClient-Win64-Shipping.exe";
            if (file_exists(ship.string())) return ship.string();
        }
    } catch (...) {
        return selected;
    }
    return selected;
}

std::string browse_for_game(HWND hwnd, const std::string& current_path)
{
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Ashes Executable (AOCClient*.exe)\0AOCClient*.exe;AOCClient-Win64-Shipping.exe\0All Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Select Ashes of Creation Executable";

    const std::wstring initDir = resolve_initial_browse_dir(current_path);
    if (!initDir.empty()) ofn.lpstrInitialDir = initDir.c_str();

    if (!GetOpenFileNameW(&ofn)) return "";
    return prefer_shipping_exe(to_utf8(szFile));
}

static ReadResult read_http_reply(HINTERNET hR, Ms deadline_at)
{
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (HttpQueryInfoA(hR, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr) &&
        status >= 300 && status < 400) {
        ReadResult out;
        out.end = ReadEnd::Redirected;
        return out;
    }
    const auto steady_now = [] {
        return std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now().time_since_epoch());
    };
    const auto read_some = [hR](char* buf, std::size_t n, Ms left) -> long {
        DWORD t = (DWORD)std::min<long long>(left.count(), kHttpWaitMs);
        InternetSetOptionA(hR, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
        DWORD got = 0;
        if (!InternetReadFile(hR, buf, (DWORD)n, &got)) return -1;
        return (long)got;
    };
    return read_reply(read_some, steady_now, deadline_at);
}

static ReadResult http_post(const AuthEndpoint& ep, const std::string& path, const std::string& body)
{
    const auto deadline_tp = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHttpDeadlineMs);
    const Ms deadline_at = std::chrono::duration_cast<Ms>(deadline_tp.time_since_epoch());

    ReadResult out;
    HINTERNET hI = InternetOpenA("AoCEmu/3.0", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
    if (!hI) return out;
    DWORD wait = kHttpWaitMs;
    InternetSetOptionA(hI, INTERNET_OPTION_CONNECT_TIMEOUT, &wait, sizeof(wait));
    InternetSetOptionA(hI, INTERNET_OPTION_SEND_TIMEOUT,    &wait, sizeof(wait));
    InternetSetOptionA(hI, INTERNET_OPTION_RECEIVE_TIMEOUT, &wait, sizeof(wait));
    HINTERNET hC = InternetConnectA(hI, ep.host.c_str(), (INTERNET_PORT)ep.port,
                                    nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hC) { InternetCloseHandle(hI); return out; }
    HINTERNET hR = HttpOpenRequestA(hC, "POST", path.c_str(), nullptr, nullptr,
                                    nullptr, INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTO_REDIRECT, 0);
    if (!hR) { InternetCloseHandle(hC); InternetCloseHandle(hI); return out; }

    std::unique_ptr<DeadlineWatch> watch;
    try {
        watch = std::make_unique<DeadlineWatch>([hR] { InternetCloseHandle(hR); }, deadline_tp);
    } catch (...) {
        watch.reset();
    }

    const char* hdrs = "Content-Type: application/json\r\n";
    if (HttpSendRequestA(hR, hdrs, (DWORD)strlen(hdrs),
                         const_cast<char*>(body.data()), (DWORD)body.size())) {
        out = read_http_reply(hR, deadline_at);
    }

    bool request_open = true;
    if (watch && watch->stop()) {
        request_open = false;
        if (out.end != ReadEnd::Complete) { out.end = ReadEnd::TimedOut; out.body.clear(); }
    }
    if (request_open) InternetCloseHandle(hR);
    InternetCloseHandle(hC);
    InternetCloseHandle(hI);
    return out;
}

static json load_server_json()
{
    try {
        std::ifstream f(exe_dir() + "config\\server.json");
        if (!f) return json::object();
        json j = json::parse(f);
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

std::string start_game(HANDLE* process)
{
    *process = nullptr;
    if (g_gameExe.empty() || !file_exists(g_gameExe))
        return "The game was not found. Use Auto-Detect or Browse to select it.";

    const ClientCommand cmd = client_command(g_gameExe, launch_settings(load_server_json()));
    std::string line = cmd.command_line;

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessA(g_gameExe.c_str(), line.data(), nullptr, nullptr,
                          FALSE, 0, nullptr,
                          cmd.working_dir.empty() ? nullptr : cmd.working_dir.c_str(), &si, &pi)) {
        return "The game could not be started (error " + std::to_string(::GetLastError()) + ").";
    }

    ::CloseHandle(pi.hThread);
    *process = pi.hProcess;
    return "";
}

void watch_game(HWND hwnd, HANDLE process)
{
    std::thread([hwnd, process] {
        ::WaitForSingleObject(process, INFINITE);
        ::CloseHandle(process);
        ::PostMessageW(hwnd, WM_GAME_EXITED, 0, 0);
    }).detach();
}

static AuthEndpoint read_auth_endpoint()
{
    return auth_endpoint(load_server_json());
}

static Reply request_token(const AuthEndpoint& server,
                           const std::string& user, const std::string& pass, bool is_reg)
{
    const std::string path = is_reg ? "/api/v1/auth/register" : "/api/v1/auth/login";
    std::string body = login_body(user, pass);
    const ReadResult got = http_post(server, path, body);
    wipe(body);
    return reply_from_read(got);
}

static bool store_token(const std::string& token)
{
    return aoc::ics_token::write(aoc::ics_token::path_in(exe_dir()), token);
}

static std::string apply_keep_preference(bool keep, const std::wstring& wuser, const std::wstring& wpass)
{
    switch (remember_action(false, true, keep)) {
    case Remember::Save:
        if (!credential_save(kCredentialTarget, SavedSignIn{wuser, wpass}))
            return "Signed in, but Windows would not save the sign-in.";
        break;
    case Remember::Delete:
        credential_remove(kCredentialTarget);
        break;
    case Remember::Leave:
        break;
    }
    return {};
}

void auth_worker(HWND hwnd, std::wstring wuser, std::wstring wpass, bool is_reg, bool keep)
{
    const std::string user = to_utf8(wuser.c_str());
    std::string pass = to_utf8(wpass.c_str());
    const Reply reply = request_token(read_auth_endpoint(), user, pass, is_reg);
    wipe(pass);

    auto* r = new AuthResult{};
    r->username = user;
    if (reply.kind != ReplyKind::Token) {
        r->ok = false;
        r->message = reply.message;
    } else if (!store_token(reply.token)) {
        r->ok = false;
        r->message = kMsgNotPassedOn;
    } else {
        r->ok = true;
        r->message = is_reg ? "Registered and signed in as " + user
                            : "Successfully signed in as " + user;
        r->note = apply_keep_preference(keep, wuser, wpass);
    }
    if (!wpass.empty()) SecureZeroMemory(wpass.data(), wpass.size() * sizeof(wchar_t));
    ::PostMessageW(hwnd, WM_AUTH_DONE, 0, (LPARAM)r);
    g_authBusy.store(false);
}

static void auto_worker(HWND hwnd, SavedSignIn saved, AuthEndpoint server)
{
    AutoOps ops;
    ops.cancelled = [] { return g_autoCancel.load(); };
    ops.show = [hwnd](const std::string& s) {
        auto* w = new std::wstring(to_wide(s));
        if (!::PostMessageW(hwnd, WM_AUTO_STATUS, 0, (LPARAM)w)) delete w;
    };
    ops.sleep = [](Ms ms) { std::this_thread::sleep_for(ms); };
    ops.now = [] {
        return std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now().time_since_epoch());
    };
    ops.request = [server](const SavedSignIn& s) {
        std::string pass = to_utf8(s.password.c_str());
        const Reply r = request_token(server, to_utf8(s.username.c_str()), pass, false);
        wipe(pass);
        return r;
    };
    ops.store_token = [](const std::string& token) { return store_token(token); };

    const AutoResult res = auto_sign_in(saved, ops);
    auto* d = new AutoDone{res.outcome, res.message, saved.username};
    if (!::PostMessageW(hwnd, WM_AUTO_DONE, 0, (LPARAM)d)) delete d;
}

void begin_auto_sign_in(HWND hwnd, const SavedSignIn& saved)
{
    // Automatic sign-in only connects to loopback endpoints so stored credentials never leave the host.
    const std::optional<AuthEndpoint> server = auto_sign_in_endpoint(load_server_json());
    if (!server) {
        g_feedback = L"Automatic sign-in only works with a server on this PC.";
        g_feedbackErr = true;
        return;
    }
    g_autoActive = true;
    g_autoUser = saved.username;
    g_autoStatus.clear();
    g_autoCancel.store(false);
    std::thread([hwnd, saved, endpoint = *server] { auto_worker(hwnd, saved, endpoint); }).detach();
}

static std::filesystem::path remembered_user_path(const wchar_t* folder = L"SandalCore")
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf) / folder / L"launcher.json";
}

void migrate_legacy_state_if_present()
{
#ifdef AOC_LAUNCHER_LEGACY_NAMES
    copy_if_missing(remembered_user_path(kLegacyUserFolder), remembered_user_path());
    credential_move(kLegacyCredentialTarget, kCredentialTarget);
#endif
}

std::wstring load_remembered_user()
{
    try {
        const auto path = remembered_user_path();
        if (path.empty()) return {};
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        const json j = json::parse(f);
        if (!j.is_object() || !j.contains("username") || !j["username"].is_string())
            return {};
        const std::wstring name = to_wide(j["username"].get<std::string>());
        if (name.empty() || name.size() > (size_t)kUserMaxChars) return {};
        for (const wchar_t c : name) {
            if (c < L' ') return {};
        }
        return name;
    } catch (...) {
        return {};
    }
}

void save_remembered_user(const std::string& name)
{
    if (name.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    try {
        const fs::path path = remembered_user_path();
        if (path.empty()) return;
        fs::create_directories(path.parent_path(), ec);
        fs::path tmp = path;
        tmp += L".tmp";

        json j;
        j["username"] = name;
        const std::string text = j.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << text;
            f.flush();
            f.close();
            if (!f) { fs::remove(tmp, ec); return; }
        }
        fs::rename(tmp, path, ec);
        if (ec) fs::remove(tmp, ec);
    } catch (...) {
        return;
    }
}

} // namespace aoc::launcher
