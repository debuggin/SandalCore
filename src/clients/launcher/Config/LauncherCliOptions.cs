// SPDX-License-Identifier: AGPL-3.0-only
global using SandalLauncher.Config;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json.Serialization;

namespace SandalLauncher.Config;

public sealed record LauncherCliOptions
{
    public const string DefaultGatewayUrl = "http://127.0.0.1:8080";
    public const string DefaultHeroName = "TestHero";
    public const int DefaultTimeoutSeconds = 30;

    public bool ShowHelp { get; init; }
    public bool ShowVersion { get; init; }
    public bool JsonOutput { get; init; }
    public bool DryRun { get; init; }
    public string GatewayUrl { get; init; } = DefaultGatewayUrl;
    public string Username { get; init; } = string.Empty;

    [JsonIgnore]
    public string Password { get; init; } = string.Empty;

    public bool RegisterTemp { get; init; }
    public bool AutoLogin { get; init; }
    public string SelectCharacter { get; init; } = string.Empty;
    public bool CreateCharacterIfMissing { get; init; }
    public bool VerifyCharSelect { get; init; }
    public bool SpawnClient { get; init; }
    public bool TerminateAfterCharSelect { get; init; }
    public bool KillExistingClient { get; init; }
    public bool DeleteTempAccountOnExit { get; init; }
    public bool Headless { get; init; }
    public bool KeepAlive { get; init; }
    public bool MockGameClient { get; init; }
    public int MockGameSleepSec { get; init; } = 300;
    public int? AutomationPort { get; init; }
    public int TimeoutSec { get; init; } = DefaultTimeoutSeconds;

    [JsonIgnore] public bool Help => ShowHelp;
    [JsonIgnore] public bool Version => ShowVersion;
    [JsonIgnore] public bool Json => JsonOutput;
    [JsonIgnore] public string Gateway => GatewayUrl;
    [JsonIgnore] public string Character => SelectCharacter;
    [JsonIgnore] public bool IsTemporaryAccount => RegisterTemp;

    [JsonIgnore]
    public string Email => string.IsNullOrWhiteSpace(Username)
        ? string.Empty
        : (Username.Contains('@', StringComparison.Ordinal) ? Username : $"{Username}@sandalcore.local");

    [JsonIgnore]
    public Uri GatewayUri => Uri.TryCreate(NormalizeGatewayUrl(GatewayUrl), UriKind.Absolute, out Uri? u)
        ? u
        : new Uri(DefaultGatewayUrl);

    [JsonIgnore]
    public bool HasAutomationFlags =>
        RegisterTemp || AutoLogin || VerifyCharSelect || !string.IsNullOrWhiteSpace(SelectCharacter) ||
        CreateCharacterIfMissing || SpawnClient || TerminateAfterCharSelect || KillExistingClient || DeleteTempAccountOnExit;

    public static string GenerateTempUsername() =>
        $"tmp_{Convert.ToHexString(RandomNumberGenerator.GetBytes(4)).ToLowerInvariant()}";

    public static string GenerateStrongPassword() =>
        $"Tmp!{Convert.ToHexString(RandomNumberGenerator.GetBytes(12))}a9";

    public static string GetUsageText() =>
        "Usage: SandalLauncher [options]\n" +
        "  --help, -h                         Show CLI usage information\n" +
        "  --version, -v                      Show launcher version\n" +
        "  --json                             Emit structured JSON output\n" +
        "  --dry-run                          Validate configuration without executing\n" +
        "  --gateway <url>                    Gateway base URL (AOC_GATEWAY_URL, default http://127.0.0.1:8080)\n" +
        "  --username, --email <user>         Account username or email (AOC_LAUNCHER_USERNAME / AOC_LAUNCHER_EMAIL)\n" +
        "  --password <pass>                  Account password (AOC_LAUNCHER_PASSWORD)\n" +
        "  --register-temp                    Register temporary account tmp_<8hex> (AOC_LAUNCHER_REGISTER_TEMP=1)\n" +
        "  --auto-login                       Automatically authenticate on startup (AOC_LAUNCHER_AUTO_LOGIN=1)\n" +
        "  --select-character <name-or-id>    Select character by name or ID (AOC_LAUNCHER_CHARACTER)\n" +
        "  --create-character-if-missing      Create character (default TestHero) if missing (AOC_LAUNCHER_CREATE_CHAR=1)\n" +
        "  --verify-char-select               Verify Character Select & Play join ticket (AOC_LAUNCHER_VERIFY_CHAR_SELECT=1)\n" +
        "  --spawn-client                     Spawn Ashes of Creation game client (AOC_LAUNCHER_SPAWN_CLIENT=1)\n" +
        "  --terminate-after-char-select      Terminate client after character select (AOC_LAUNCHER_TERMINATE_AFTER_CHAR_SELECT=1)\n" +
        "  --kill-existing-client             Kill any running Ashes client processes (AOC_LAUNCHER_KILL_EXISTING_CLIENT=1)\n" +
        "  --delete-temp-account-on-exit      Delete temporary account and purge creds on exit (AOC_LAUNCHER_DELETE_TEMP_ON_EXIT=1)\n" +
        "  --headless                         Run CLI automation without desktop GUI window (AOC_LAUNCHER_HEADLESS=1)\n" +
        "  --keep-alive                       Keep automation server alive until /automation/close-launcher (AOC_LAUNCHER_KEEP_ALIVE=1)\n" +
        "  --mock-game-client [sec]           Run headless mock game client process (AOC_LAUNCHER_MOCK_GAME_CLIENT=1)\n" +
        "  --automation-port <port>           Automation listener TCP port (AOC_LAUNCHER_AUTOMATION_PORT)\n" +
        "  --timeout-sec <sec>                Operation timeout in seconds (default 30)";

    public static LauncherCliOptions Parse(string[]? args, IReadOnlyDictionary<string, string?>? envOverrides = null) =>
        Parse(args, key => envOverrides is not null && envOverrides.TryGetValue(key, out string? v) ? v : Environment.GetEnvironmentVariable(key));

    public static LauncherCliOptions Parse(string[]? args, Func<string, string?>? envReader)
    {
        Func<string, string?> env = envReader ?? Environment.GetEnvironmentVariable;
        var builder = new Builder(env);
        builder.ApplyArgs(args ?? []);
        return builder.Build();
    }

    internal static string NormalizeGatewayUrl(string? raw)
    {
        if (string.IsNullOrWhiteSpace(raw)) return DefaultGatewayUrl;
        string trimmed = raw.Trim().TrimEnd('/');
        if (!trimmed.StartsWith("http://", StringComparison.OrdinalIgnoreCase) &&
            !trimmed.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
        {
            return $"http://{trimmed}";
        }
        return trimmed;
    }

    private sealed class Builder
    {
        public bool ShowHelp, ShowVersion, JsonOutput, DryRun, RegisterTemp, AutoLogin;
        public bool CreateCharIfMissing, VerifyCharSelect, SpawnClient, TerminateAfterCharSelect, KillExistingClient, DeleteTempOnExit;
        public bool Headless, KeepAlive, MockGameClient;
        public int MockGameSleepSec = 300;
        public string GatewayUrl, Username, Password, SelectCharacter;
        public int? AutomationPort;
        public int TimeoutSec = DefaultTimeoutSeconds;

        public Builder(Func<string, string?> env)
        {
            GatewayUrl = NormalizeGatewayUrl(ReadEnv(env, "AOC_GATEWAY_URL"));
            Username = FirstNonEmpty(ReadEnv(env, "AOC_LAUNCHER_USERNAME"), ReadEnv(env, "AOC_LAUNCHER_EMAIL"));
            Password = ReadEnv(env, "AOC_LAUNCHER_PASSWORD");
            RegisterTemp = ReadBoolEnv(env, "AOC_LAUNCHER_REGISTER_TEMP");
            AutoLogin = ReadBoolEnv(env, "AOC_LAUNCHER_AUTO_LOGIN");
            SelectCharacter = ReadEnv(env, "AOC_LAUNCHER_CHARACTER");
            CreateCharIfMissing = ReadBoolEnv(env, "AOC_LAUNCHER_CREATE_CHAR");
            VerifyCharSelect = ReadBoolEnv(env, "AOC_LAUNCHER_VERIFY_CHAR_SELECT");
            SpawnClient = ReadBoolEnv(env, "AOC_LAUNCHER_SPAWN_CLIENT");
            TerminateAfterCharSelect = ReadBoolEnv(env, "AOC_LAUNCHER_TERMINATE_AFTER_CHAR_SELECT");
            KillExistingClient = ReadBoolEnv(env, "AOC_LAUNCHER_KILL_EXISTING_CLIENT");
            DeleteTempOnExit = ReadBoolEnv(env, "AOC_LAUNCHER_DELETE_TEMP_ON_EXIT");
            Headless = ReadBoolEnv(env, "AOC_LAUNCHER_HEADLESS");
            KeepAlive = ReadBoolEnv(env, "AOC_LAUNCHER_KEEP_ALIVE");
            MockGameClient = ReadBoolEnv(env, "AOC_LAUNCHER_MOCK_GAME_CLIENT");
            if (int.TryParse(ReadEnv(env, "AOC_LAUNCHER_AUTOMATION_PORT"), NumberStyles.Integer, CultureInfo.InvariantCulture, out int port) && port is > 0 and <= 65535)
                AutomationPort = port;
            if (int.TryParse(ReadEnv(env, "AOC_LAUNCHER_TIMEOUT_SEC"), NumberStyles.Integer, CultureInfo.InvariantCulture, out int timeout) && timeout > 0)
                TimeoutSec = timeout;
        }

        public void ApplyArgs(string[] args)
        {
            for (int i = 0; i < args.Length; i++)
            {
                (string flag, string? inlineVal) = SplitFlag(args[i]);
                if (TryApplyLifecycleFlag(flag, inlineVal, args, ref i)) continue;
                if (TryApplyBooleanFlag(flag, inlineVal)) continue;
                string val = inlineVal ?? (i + 1 < args.Length ? args[++i] : string.Empty);
                ApplyValueFlag(flag, val);
            }
        }

        private bool TryApplyLifecycleFlag(string flag, string? inlineVal, string[] args, ref int i)
        {
            if (flag == "--headless")
            {
                Headless = inlineVal is null || ParseBool(inlineVal, true);
                return true;
            }
            if (flag == "--keep-alive")
            {
                KeepAlive = inlineVal is null || ParseBool(inlineVal, true);
                return true;
            }
            if (flag == "--mock-game-client")
            {
                MockGameClient = true;
                string? next = inlineVal ?? (i + 1 < args.Length && !args[i + 1].StartsWith('-') ? args[i + 1] : null);
                if (next is not null && int.TryParse(next, NumberStyles.Integer, CultureInfo.InvariantCulture, out int sec) && sec > 0)
                {
                    MockGameSleepSec = sec;
                    if (inlineVal is null) i++;
                }
                return true;
            }
            return false;
        }

        private bool TryApplyBooleanFlag(string flag, string? inlineVal)
        {
            bool b = inlineVal is null || ParseBool(inlineVal, true);
            switch (flag)
            {
                case "--help" or "-h": ShowHelp = b; return true;
                case "--version" or "-v": ShowVersion = b; return true;
                case "--json": JsonOutput = b; return true;
                case "--dry-run": DryRun = b; return true;
                case "--register-temp": RegisterTemp = b; return true;
                case "--auto-login": AutoLogin = b; return true;
                case "--create-character-if-missing": CreateCharIfMissing = b; return true;
                case "--verify-char-select": VerifyCharSelect = b; return true;
                case "--spawn-client": SpawnClient = b; return true;
                case "--terminate-after-char-select": TerminateAfterCharSelect = b; return true;
                case "--kill-existing-client": KillExistingClient = b; return true;
                case "--delete-temp-account-on-exit": DeleteTempOnExit = b; return true;
                default: return false;
            }
        }

        private void ApplyValueFlag(string flag, string val)
        {
            switch (flag)
            {
                case "--gateway" or "--gateway-url": GatewayUrl = NormalizeGatewayUrl(val); break;
                case "--username" or "--email": Username = val.Trim(); break;
                case "--password": Password = val; break;
                case "--select-character": SelectCharacter = val.Trim(); break;
                case "--automation-port":
                    if (int.TryParse(val, NumberStyles.Integer, CultureInfo.InvariantCulture, out int port) && port is > 0 and <= 65535)
                        AutomationPort = port;
                    break;
                case "--timeout-sec":
                    if (int.TryParse(val, NumberStyles.Integer, CultureInfo.InvariantCulture, out int sec) && sec > 0)
                        TimeoutSec = sec;
                    break;
            }
        }

        public LauncherCliOptions Build()
        {
            string resolvedUser = Username;
            string resolvedPass = Password;
            if (RegisterTemp)
            {
                if (string.IsNullOrWhiteSpace(resolvedUser)) resolvedUser = GenerateTempUsername();
                if (string.IsNullOrEmpty(resolvedPass)) resolvedPass = GenerateStrongPassword();
            }
            string resolvedChar = SelectCharacter;
            if (string.IsNullOrWhiteSpace(resolvedChar) && (CreateCharIfMissing || VerifyCharSelect))
                resolvedChar = DefaultHeroName;
            return new LauncherCliOptions
            {
                ShowHelp = ShowHelp, ShowVersion = ShowVersion, JsonOutput = JsonOutput, DryRun = DryRun,
                GatewayUrl = NormalizeGatewayUrl(GatewayUrl), Username = resolvedUser, Password = resolvedPass,
                RegisterTemp = RegisterTemp, AutoLogin = AutoLogin, SelectCharacter = resolvedChar,
                CreateCharacterIfMissing = CreateCharIfMissing, VerifyCharSelect = VerifyCharSelect,
                SpawnClient = SpawnClient, TerminateAfterCharSelect = TerminateAfterCharSelect,
                KillExistingClient = KillExistingClient, DeleteTempAccountOnExit = DeleteTempOnExit,
                Headless = Headless, KeepAlive = KeepAlive, MockGameClient = MockGameClient, MockGameSleepSec = Math.Max(1, MockGameSleepSec),
                AutomationPort = AutomationPort, TimeoutSec = Math.Max(1, TimeoutSec)
            };
        }

        private static (string Flag, string? InlineVal) SplitFlag(string raw)
        {
            int eq = raw.IndexOf('=', StringComparison.Ordinal);
            return eq > 0 ? (raw[..eq].Trim(), raw[(eq + 1)..]) : (raw.Trim(), null);
        }

        private static string ReadEnv(Func<string, string?> env, string key) => env(key)?.Trim() ?? string.Empty;
        private static bool ReadBoolEnv(Func<string, string?> env, string key) => ParseBool(env(key), false);
        private static string FirstNonEmpty(string a, string b) => !string.IsNullOrWhiteSpace(a) ? a : b;
        private static bool ParseBool(string? v, bool fallback) =>
            string.IsNullOrWhiteSpace(v) ? fallback : v.Trim() is "1" || v.Trim().Equals("true", StringComparison.OrdinalIgnoreCase) || v.Trim().Equals("yes", StringComparison.OrdinalIgnoreCase);
    }
}
