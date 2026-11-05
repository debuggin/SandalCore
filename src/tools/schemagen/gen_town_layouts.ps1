#!/usr/bin/env pwsh
#Requires -Version 7.0
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 SandalCore contributors.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Import-SqliteAssemblies {
    param([string]$BaseDir, [string]$NativePath)
    if ($NativePath -and (Test-Path -LiteralPath $NativePath)) {
        [void][System.Runtime.InteropServices.NativeLibrary]::Load($NativePath)
    }
    foreach ($dll in @('SQLitePCLRaw.core.dll', 'SQLitePCLRaw.provider.e_sqlite3.dll', 'SQLitePCLRaw.batteries_v2.dll', 'Microsoft.Data.Sqlite.dll')) {
        $full = Join-Path $BaseDir $dll
        if (Test-Path -LiteralPath $full) { Add-Type -LiteralPath $full }
    }
    [SQLitePCL.Batteries_V2]::Init()
}

function Initialize-ManagedSqlite {
    param([string]$RepoRoot)
    if ('Microsoft.Data.Sqlite.SqliteConnection' -as [type]) { return }
    $rid = if ($IsWindows) { 'win-x64' } elseif ($IsLinux) { 'linux-x64' } else { 'osx-x64' }
    $binDir = Get-ChildItem -Path (Join-Path $RepoRoot 'build/dotnet/bin') -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($binDir) {
        $dir = $binDir.DirectoryName
        $native = Get-ChildItem -Path $dir -Recurse -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match $rid } | Select-Object -First 1
        Import-SqliteAssemblies -BaseDir $dir -NativePath ($native ? $native.FullName : '')
        return
    }
    $nuget = @($env:NUGET_PACKAGES, (Join-Path $HOME '.nuget/packages'), (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.nuget/packages')) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (-not $nuget) { throw 'Microsoft.Data.Sqlite assemblies not found in build/dotnet/bin or NuGet cache.' }
    $nativePkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.lib.e_sqlite3') -Recurse -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match $rid } | Select-Object -Last 1
    $corePkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.core') -Recurse -Filter 'SQLitePCLRaw.core.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $provPkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.provider.e_sqlite3') -Recurse -Filter 'SQLitePCLRaw.provider.e_sqlite3.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $batPkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.bundle_e_sqlite3') -Recurse -Filter 'SQLitePCLRaw.batteries_v2.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $mdsPkg = Get-ChildItem -Path (Join-Path $nuget 'microsoft.data.sqlite.core') -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'net8\.0|netstandard2\.0' } | Select-Object -Last 1
    if ($nativePkg) { [void][System.Runtime.InteropServices.NativeLibrary]::Load($nativePkg.FullName) }
    foreach ($pkg in @($corePkg, $provPkg, $batPkg, $mdsPkg)) { Add-Type -LiteralPath $pkg.FullName }
    [SQLitePCL.Batteries_V2]::Init()
}

function Parse-CliOptions {
    param([string]$RepoRoot, [string[]]$CliArgs)
    $opts = [ordered]@{
        ContentDb = Join-Path $RepoRoot 'dist/content/content.db'
        Seed      = Join-Path $RepoRoot 'src/config/seeds/citynode_seed.json'
        Out       = Join-Path $RepoRoot 'dist/content/town_layout_items.tsv'
        Check     = $false
    }
    $i = 0
    while ($i -lt $CliArgs.Count) {
        $arg = $CliArgs[$i]
        if ($arg -eq '--content-db' -and ($i + 1) -lt $CliArgs.Count) { $opts.ContentDb = $CliArgs[++$i] }
        elseif ($arg -eq '--seed' -and ($i + 1) -lt $CliArgs.Count) { $opts.Seed = $CliArgs[++$i] }
        elseif ($arg -eq '--out' -and ($i + 1) -lt $CliArgs.Count) { $opts.Out = $CliArgs[++$i] }
        elseif ($arg -eq '--check') { $opts.Check = $true }
        else { throw "gen_town_layouts: unknown argument '$arg'" }
        $i++
    }
    return $opts
}

function Initialize-TownLayoutEngine {
    if ('SandalCore.TownLayouts.TownLayoutBuilder' -as [type]) { return }
    $mdsAsm = ([Microsoft.Data.Sqlite.SqliteConnection]).Assembly.Location
    $sqlPclAsm = ([SQLitePCL.Batteries_V2]).Assembly.Location
    $sqlCoreAsm = ([SQLitePCL.raw]).Assembly.Location
    $refs = @(
        $mdsAsm,
        $sqlPclAsm,
        $sqlCoreAsm,
        'netstandard',
        'System.Runtime',
        'System.Collections',
        'System.IO.Compression',
        'System.Linq',
        'System.Memory',
        'System.Text.Json',
        'System.Text.RegularExpressions',
        'System.Data.Common',
        'System.ComponentModel.Primitives'
    )
    Add-Type -TypeDefinition $script:TownLayoutCsSource -ReferencedAssemblies $refs -IgnoreWarnings -WarningAction SilentlyContinue
}

$script:TownLayoutCsSource = @'
#nullable enable
#pragma warning disable 1701, 1702
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Data.Sqlite;

namespace SandalCore.TownLayouts
{
    public sealed class TownLayoutBuilder
    {
        private static readonly long[] Bowls = new long[] {
            108646108518008L, 108646121231993L, 108646608033403L, 108646610851452L, 108646615701117L
        };
        private static readonly string[] NodeTypes = new string[] { "Default", "Scientific", "Divine", "Martial", "Economic" };
        private static readonly string[] Biomes = new string[] {
            "Default", "AlpineForest", "AncientForest", "ArcticTundra", "Asian", "Badlands", "Desert", "Fjords", "Glacier",
            "Grasslands", "Iceland", "Jungle", "MagicalForest", "RedwoodForest", "Riverlands", "Swamp", "Taiga",
            "TabletopMountains", "Tropics", "Volcano", "UnderRealm", "Highlands"
        };
        private static readonly string[] States = new string[] {
            "Empty", "UnderConstruction", "Operational", "Disarray", "Damaged", "Repairing", "Destroyed", "Demolition"
        };
        private static readonly Dictionary<int, string> StateList = new Dictionary<int, string> {
            { 1, "ConstructionAssetSetIds" }, { 2, "OperationalAssetSetIds" }, { 3, "DisarrayAssetSetIds" },
            { 4, "DamagedAssetSetIds" }, { 5, "RepairingAssetSetIds" }, { 6, "DestroyedAssetSetIds" }, { 7, "DemolitionAssetSetIds" }
        };
        private static readonly Regex StepRx = new Regex(@"canCharacterStepUpOn""\s*:\s*""(\w+)""", RegexOptions.Compiled);
        private static readonly Regex NonAlnumRx = new Regex(@"[^0-9a-z]", RegexOptions.Compiled);

        private sealed class BlobEntry
        {
            public string Schema = "";
            public string Name = "";
            public byte[] Data = Array.Empty<byte>();
        }

        private sealed class DefFilter
        {
            public int MinLevel = 0;
            public int MaxLevel = 6;
            public HashSet<string> Cultures = new HashSet<string>();
            public HashSet<string> NodeTypes = new HashSet<string>();
            public HashSet<string> Seasons = new HashSet<string>();
            public bool AllBiomes = false;
            public HashSet<string> Biomes = new HashSet<string>();
        }

        private sealed class AssetGroup
        {
            public string Kind = "";
            public string Key = "";
            public List<double[]> Mats = new List<double[]>();
            public List<int> Steps = new List<int>();
        }

        private sealed class ParsedDef
        {
            public string Name = "";
            public DefFilter Filter = new DefFilter();
            public List<AssetGroup> Assets = new List<AssetGroup>();
        }

        private sealed class NodeInfo
        {
            public long Guid;
            public string Name = "";
            public string NodeName = "";
            public double X, Y, Z, Yaw;
            public string Layout = "";
            public string Type = "";
            public int Level;
            public JsonElement RecRoot;
        }

        private readonly Dictionary<long, BlobEntry> _blobs = new Dictionary<long, BlobEntry>();
        private readonly Dictionary<long, (string Schema, string Name, JsonDocument Doc)> _cache = new Dictionary<long, (string, string, JsonDocument)>();
        private readonly Dictionary<long, ParsedDef?> _defs = new Dictionary<long, ParsedDef?>();
        private readonly Dictionary<(long, int, int), bool> _valid = new Dictionary<(long, int, int), bool>();

        private static JsonDocument DecompressJson(byte[] gz)
        {
            using var ms = new MemoryStream(gz);
            using var gzStream = new GZipStream(ms, CompressionMode.Decompress);
            return JsonDocument.Parse(gzStream);
        }

        private (string Schema, string Name, JsonDocument Doc)? GetRaw(long guid)
        {
            if (!_blobs.TryGetValue(guid, out var b)) return null;
            return (b.Schema, b.Name, DecompressJson(b.Data));
        }

        private (string Schema, string Name, JsonElement Root)? GetCached(long guid)
        {
            if (!_cache.TryGetValue(guid, out var c))
            {
                var raw = GetRaw(guid);
                if (raw == null) return null;
                c = raw.Value;
                _cache[guid] = c;
            }
            return (c.Schema, c.Name, c.Doc.RootElement);
        }

        private static double GetDouble(JsonElement obj, string prop, double defVal)
        {
            if (obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(prop, out var v) && v.ValueKind == JsonValueKind.Number)
                return v.GetDouble();
            return defVal;
        }

        private static long GetGuidProp(JsonElement obj, string prop)
        {
            if (obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(prop, out var sub) &&
                sub.ValueKind == JsonValueKind.Object && sub.TryGetProperty("Guid", out var g) && g.ValueKind == JsonValueKind.Number)
                return g.GetInt64();
            return 0L;
        }

        private static double[] Matrix(JsonElement t)
        {
            double rx = 0.0, ry = 0.0, rz = 0.0, rw = 1.0;
            double sx = 1.0, sy = 1.0, sz = 1.0;
            double px = 0.0, py = 0.0, pz = 0.0;
            if (t.ValueKind == JsonValueKind.Object)
            {
                if (t.TryGetProperty("Rotation", out var r))
                {
                    rx = GetDouble(r, "X", 0.0); ry = GetDouble(r, "Y", 0.0); rz = GetDouble(r, "Z", 0.0); rw = GetDouble(r, "W", 1.0);
                }
                if (t.TryGetProperty("Scale3D", out var s))
                {
                    sx = GetDouble(s, "X", 1.0); sy = GetDouble(s, "Y", 1.0); sz = GetDouble(s, "Z", 1.0);
                }
                if (t.TryGetProperty("Translation", out var p))
                {
                    px = GetDouble(p, "X", 0.0); py = GetDouble(p, "Y", 0.0); pz = GetDouble(p, "Z", 0.0);
                }
            }
            double n = Math.Sqrt(rx * rx + ry * ry + rz * rz + rw * rw);
            if (n == 0.0) n = 1.0;
            double x = rx / n, y = ry / n, z = rz / n, w = rw / n;
            return new double[] {
                (1.0 - 2.0 * (y * y + z * z)) * sx, 2.0 * (x * y - z * w) * sy, 2.0 * (x * z + y * w) * sz,
                2.0 * (x * y + z * w) * sx, (1.0 - 2.0 * (x * x + z * z)) * sy, 2.0 * (y * z - x * w) * sz,
                2.0 * (x * z - y * w) * sx, 2.0 * (y * z + x * w) * sy, (1.0 - 2.0 * (x * x + y * y)) * sz,
                px, py, pz
            };
        }

        private static double[] Mul(double[] a, double[] b)
        {
            return new double[] {
                a[0] * b[0] + a[1] * b[3] + a[2] * b[6], a[0] * b[1] + a[1] * b[4] + a[2] * b[7], a[0] * b[2] + a[1] * b[5] + a[2] * b[8],
                a[3] * b[0] + a[4] * b[3] + a[5] * b[6], a[3] * b[1] + a[4] * b[4] + a[5] * b[7], a[3] * b[2] + a[4] * b[5] + a[5] * b[8],
                a[6] * b[0] + a[7] * b[3] + a[8] * b[6], a[6] * b[1] + a[7] * b[4] + a[8] * b[7], a[6] * b[2] + a[7] * b[5] + a[8] * b[8],
                a[0] * b[9] + a[1] * b[10] + a[2] * b[11] + a[9],
                a[3] * b[9] + a[4] * b[10] + a[5] * b[11] + a[10],
                a[6] * b[9] + a[7] * b[10] + a[8] * b[11] + a[11]
            };
        }

        private static double[] FrameMatrix(double x, double y, double z, double yawDegrees)
        {
            double a = yawDegrees * (Math.PI / 180.0);
            double c = Math.Cos(a), s = Math.Sin(a);
            return new double[] { c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0, x, y, z };
        }

        private static HashSet<string> ReadStringSet(JsonElement d, string key)
        {
            var set = new HashSet<string>();
            if (d.ValueKind == JsonValueKind.Object && d.TryGetProperty(key, out var arr) && arr.ValueKind == JsonValueKind.Array)
            {
                foreach (var el in arr.EnumerateArray())
                {
                    if (el.ValueKind == JsonValueKind.String) set.Add(el.GetString() ?? "");
                }
            }
            return set;
        }

        private static bool IsDefValid(DefFilter f, int level, int ntype)
        {
            if (level < f.MinLevel || level > f.MaxLevel) return false;
            if (!f.Cultures.Contains("ECharacterRace::Kaelar")) return false;
            if (!f.NodeTypes.Contains("ENodeType::" + NodeTypes[ntype])) return false;
            if (!f.Seasons.Contains("ESeasons::Spring")) return false;
            return f.AllBiomes || f.Biomes.Contains("EBiomeType::Riverlands");
        }

        private ParsedDef? GetDefinition(long guid)
        {
            if (_defs.TryGetValue(guid, out var cached)) return cached;
            var raw = GetRaw(guid);
            if (raw == null) { _defs[guid] = null; return null; }
            using var doc = raw.Value.Doc;
            var d = doc.RootElement;
            var parsed = new ParsedDef { Name = raw.Value.Name };
            parsed.Filter.MinLevel = (d.TryGetProperty("NodeLevelMinimum", out var mn) && mn.ValueKind == JsonValueKind.Number) ? mn.GetInt32() : 0;
            parsed.Filter.MaxLevel = (d.TryGetProperty("NodeLevelMaximum", out var mx) && mx.ValueKind == JsonValueKind.Number) ? mx.GetInt32() : 6;
            parsed.Filter.Cultures = ReadStringSet(d, "ValidCultures");
            parsed.Filter.NodeTypes = ReadStringSet(d, "ValidNodeTypes");
            parsed.Filter.Seasons = ReadStringSet(d, "ValidSeasons");
            parsed.Filter.AllBiomes = d.TryGetProperty("bAllBiomes", out var ab) && ab.ValueKind == JsonValueKind.True;
            parsed.Filter.Biomes = ReadStringSet(d, "ValidBiomes");

            if (d.TryGetProperty("ContainedAssets", out var ca) && ca.ValueKind == JsonValueKind.Array)
            {
                foreach (var a in ca.EnumerateArray())
                {
                    string key = (a.TryGetProperty("Key", out var kEl) && kEl.ValueKind == JsonValueKind.String) ? (kEl.GetString() ?? "") : "";
                    bool isWorld = key.Contains("/Level_Instances/") || key.StartsWith("/Game/Levels/NodeLayouts");
                    var group = new AssetGroup { Key = key };
                    string? kind = null;
                    if (a.TryGetProperty("Value", out var valEl) && valEl.ValueKind == JsonValueKind.Object &&
                        valEl.TryGetProperty("InstanceData", out var instArr) && instArr.ValueKind == JsonValueKind.Array)
                    {
                        foreach (var inst in instArr.EnumerateArray())
                        {
                            string tyRaw = (inst.TryGetProperty("Type", out var tEl) && tEl.ValueKind == JsonValueKind.String) ? (tEl.GetString() ?? "") : "";
                            int idx = tyRaw.LastIndexOf("::", StringComparison.Ordinal);
                            string ty = idx >= 0 ? tyRaw.Substring(idx + 2) : tyRaw;
                            string k;
                            if (ty == "StaticMesh") k = "M";
                            else if (isWorld) k = "L";
                            else continue;
                            if (kind == null) kind = k;
                            else if (kind != k) throw new InvalidOperationException($"gen_town_layouts: {key} mixes static meshes and level instances ({raw.Value.Name})");
                            JsonElement trEl = inst.TryGetProperty("Transform", out var tProp) ? tProp : default;
                            group.Mats.Add(Matrix(trEl));
                            string ds = (inst.TryGetProperty("DataString", out var dsEl) && dsEl.ValueKind == JsonValueKind.String) ? (dsEl.GetString() ?? "") : "";
                            var m = StepRx.Match(ds);
                            group.Steps.Add((m.Success && m.Groups[1].Value == "ECB_No") ? 0 : 1);
                        }
                    }
                    if (kind != null)
                    {
                        group.Kind = kind;
                        parsed.Assets.Add(group);
                    }
                }
            }
            _defs[guid] = parsed;
            return parsed;
        }

        private List<(long DefGuid, double[] Transform)> GetValidDefs(long setGuid, int level, int ntype)
        {
            var s = GetCached(setGuid);
            var outList = new List<(long, double[])>();
            if (s == null) return outList;
            if (s.Value.Root.TryGetProperty("ContainedAssetDefinitions", out var cad) && cad.ValueKind == JsonValueKind.Array)
            {
                foreach (var cd in cad.EnumerateArray())
                {
                    long g = GetGuidProp(cd, "Asset");
                    var d = GetDefinition(g);
                    if (d == null || !IsDefValid(d.Filter, level, ntype)) continue;
                    JsonElement trEl = cd.TryGetProperty("Transform", out var tProp) ? tProp : default;
                    outList.Add((g, Matrix(trEl)));
                }
            }
            return outList;
        }

        private bool IsSetValid(long setGuid, int level, int ntype)
        {
            var key = (setGuid, level, ntype);
            if (!_valid.TryGetValue(key, out bool v))
            {
                v = GetValidDefs(setGuid, level, ntype).Count > 0;
                _valid[key] = v;
            }
            return v;
        }

        private (List<(double[] Transform, long SetGuid)> Items, int NType) GetItems(JsonElement nodeRoot, int level)
        {
            string ntRaw = nodeRoot.GetProperty("NodeType").GetString() ?? "";
            int ntype = Array.IndexOf(NodeTypes, ntRaw.Split("::")[1]);
            long layoutGuid = GetGuidProp(nodeRoot, "LayoutId");
            var lay = GetCached(layoutGuid)!.Value.Root;
            var outList = new List<(double[], long)>();
            if (lay.TryGetProperty("Props", out var props) && props.ValueKind == JsonValueKind.Array)
            {
                foreach (var p in props.EnumerateArray())
                {
                    long sg = GetGuidProp(p, "AssetSet");
                    if (IsSetValid(sg, level, ntype))
                        outList.Add((Matrix(p.GetProperty("Transform")), sg));
                }
            }
            if (lay.TryGetProperty("PlotsPerNodeLevel", out var ppnl) && ppnl.ValueKind == JsonValueKind.Array)
            {
                foreach (var e in ppnl.EnumerateArray())
                {
                    if (e.GetProperty("Key").GetInt32() != level) continue;
                    var plots = e.GetProperty("Value").GetProperty("Plots");
                    foreach (var pl in plots.EnumerateArray())
                    {
                        long plotGuid = GetGuidProp(pl, "Plot");
                        var pr = GetCached(plotGuid)!.Value;
                        var pd = pr.Root;
                        if (!pd.TryGetProperty("PreConstruct", out var pc) || pc.ValueKind != JsonValueKind.True) continue;
                        string stRaw = pd.GetProperty("StartingState").GetString() ?? "";
                        int state = Array.IndexOf(States, stRaw.Split("::")[1]);
                        var ids = new List<long>();
                        if (pd.TryGetProperty("BuildingsIds", out var bArr) && bArr.ValueKind == JsonValueKind.Array)
                        {
                            foreach (var bEl in bArr.EnumerateArray())
                            {
                                if (bEl.TryGetProperty("Guid", out var gEl) && gEl.ValueKind == JsonValueKind.Number && gEl.GetInt64() != 0)
                                    ids.Add(gEl.GetInt64());
                            }
                        }
                        long bg = ids.Count == 1 ? ids[0] : GetGuidProp(pd, "EmptyBuildingId");
                        if (bg == 0 && ids.Count > 0) bg = ids[0];
                        if (bg == 0) continue;
                        int st = pr.Name.Contains("ConstructionProject_Large") ? state : 2;
                        var bRec = GetCached(bg)!.Value.Root;
                        var setsRec = GetCached(GetGuidProp(bRec, "BuildingAssetSetId"))!.Value.Root;
                        if (StateList.TryGetValue(st, out string? listName) && setsRec.TryGetProperty(listName, out var sArr) && sArr.ValueKind == JsonValueKind.Array)
                        {
                            foreach (var x in sArr.EnumerateArray())
                            {
                                long sg = x.GetProperty("Guid").GetInt64();
                                if (IsSetValid(sg, level, ntype))
                                    outList.Add((Matrix(pl.GetProperty("Transform")), sg));
                            }
                        }
                    }
                }
            }
            return (outList, ntype);
        }

        private static string Norm(string name) => NonAlnumRx.Replace(name.ToLowerInvariant(), "");

        private static Dictionary<long, int> ReadSeedLevels(string? seedPath, List<NodeInfo> nodes)
        {
            var levels = nodes.ToDictionary(n => n.Guid, _ => 0);
            if (string.IsNullOrEmpty(seedPath) || !File.Exists(seedPath)) return levels;
            string jsonText = File.ReadAllText(seedPath, Encoding.UTF8);
            using var doc = JsonDocument.Parse(jsonText);
            var byName = new Dictionary<string, long>();
            foreach (var n in nodes)
            {
                byName[Norm(n.Name)] = n.Guid;
                byName[Norm(n.NodeName)] = n.Guid;
                byName[n.Guid.ToString(CultureInfo.InvariantCulture)] = n.Guid;
                if (n.Name.StartsWith("Verra_", StringComparison.Ordinal))
                    byName[Norm(n.Name.Substring(6))] = n.Guid;
            }
            if (doc.RootElement.TryGetProperty("nodes", out var nodesObj) && nodesObj.ValueKind == JsonValueKind.Object)
            {
                foreach (var prop in nodesObj.EnumerateObject())
                {
                    if (!byName.TryGetValue(Norm(prop.Name), out long guid)) continue;
                    if (prop.Value.ValueKind != JsonValueKind.Object || !prop.Value.TryGetProperty("level", out var lvlEl) ||
                        lvlEl.ValueKind != JsonValueKind.Number || !lvlEl.TryGetInt32(out int lvl) || lvl < 0 || lvl >= 7)
                    {
                        throw new InvalidOperationException($"gen_town_layouts: {seedPath}: node \"{prop.Name}\" needs a level from 0 to 6");
                    }
                    levels[guid] = lvl;
                }
            }
            return levels;
        }

        private static string FormatNum(double v, int digits)
        {
            string s = v.ToString("F" + digits.ToString(CultureInfo.InvariantCulture), CultureInfo.InvariantCulture);
            if (s.StartsWith("-", StringComparison.Ordinal) && double.Parse(s, CultureInfo.InvariantCulture) == 0.0)
                s = s.Substring(1);
            if (s.IndexOf('.') >= 0)
                s = s.TrimEnd('0').TrimEnd('.');
            return s;
        }

        private static string FormatRotNum(double v)
        {
            string s = v.ToString("g9", CultureInfo.InvariantCulture);
            if (double.Parse(s, CultureInfo.InvariantCulture) == 0.0) return "0";
            return s;
        }

        public (string Text, List<string> SummaryLines) Build(string dbPath, string? seedPath)
        {
            var cb = new SqliteConnectionStringBuilder { DataSource = Path.GetFullPath(dbPath), Mode = SqliteOpenMode.ReadOnly };
            using var conn = new SqliteConnection(cb.ToString());
            conn.Open();

            using (var cmd = conn.CreateCommand())
            {
                cmd.CommandText = "SELECT guid, schema, name, data FROM record WHERE schema IN ('CityNodeRecordBase', 'LayoutRecordBase', 'NodeAssetSetRecordBase', 'NodeAssetDefinitionRecordBase', 'BuildingNodeAssetSetsRecordBase', 'ServiceBuildingPlotRecordBase', 'BuildingRecordBase')";
                using var r = cmd.ExecuteReader();
                while (r.Read())
                {
                    _blobs[r.GetInt64(0)] = new BlobEntry {
                        Schema = r.GetString(1),
                        Name = r.GetString(2),
                        Data = (byte[])r.GetValue(3)
                    };
                }
            }

            var zoiBiome = new Dictionary<long, int>();
            using (var cmd = conn.CreateCommand())
            {
                cmd.CommandText = "SELECT node_guid, biome_ord FROM city_zoi";
                using var r = cmd.ExecuteReader();
                while (r.Read()) zoiBiome[r.GetInt64(0)] = r.GetInt32(1);
            }

            var nodes = new List<NodeInfo>();
            foreach (long guid in Bowls)
            {
                using var cmd = conn.CreateCommand();
                cmd.CommandText = "SELECT guid, name, x, y, z, yaw, layout_guid, node_name FROM city_node WHERE guid = $g";
                cmd.Parameters.AddWithValue("$g", guid);
                using var r = cmd.ExecuteReader();
                if (!r.Read()) throw new InvalidOperationException($"gen_town_layouts: city_node has no record {guid} (content.db is too old?)");
                var nr = GetCached(guid)!.Value;
                var layoutName = GetCached(r.GetInt64(6))!.Value.Name;
                string name = r.GetString(1);
                if (zoiBiome.TryGetValue(guid, out int biome) && Biomes[biome] != "Riverlands")
                    throw new InvalidOperationException($"gen_town_layouts: {name} is not a Riverlands node (biome {Biomes[biome]})");
                nodes.Add(new NodeInfo {
                    Guid = guid, Name = name, NodeName = r.GetString(7),
                    X = r.GetDouble(2), Y = r.GetDouble(3), Z = r.GetDouble(4), Yaw = r.GetDouble(5),
                    Layout = layoutName, RecRoot = nr.Root
                });
            }

            var levels = ReadSeedLevels(seedPath, nodes);
            foreach (var n in nodes) n.Level = levels[n.Guid];

            var assetIds = new Dictionary<string, string>();
            var rows = new List<(long Guid, string Key, string Kind, int Mask, int Step, double[] Mat)>();
            var summaryLines = new List<string>();

            foreach (var n in nodes)
            {
                double[] frame = FrameMatrix(n.X, n.Y, n.Z, n.Yaw);
                var mergedOrder = new List<(string Key, int Mask, int Step, double[] Mat, string Kind)>();
                var mergedIndex = new Dictionary<string, int>();

                for (int level = 0; level < 7; level++)
                {
                    var (items, ntype) = GetItems(n.RecRoot, level);
                    n.Type = NodeTypes[ntype];
                    foreach (var (tr, sg) in items)
                    {
                        double[] itemMat = Mul(frame, tr);
                        foreach (var (dg, mtr) in GetValidDefs(sg, level, ntype))
                        {
                            var d = GetDefinition(dg)!;
                            double[] baseMat = Mul(itemMat, mtr);
                            foreach (var ag in d.Assets)
                            {
                                for (int i = 0; i < ag.Mats.Count; i++)
                                {
                                    double[] m = Mul(baseMat, ag.Mats[i]);
                                    var sb = new StringBuilder(ag.Key, ag.Key.Length + 128);
                                    for (int j = 0; j < 9; j++) { sb.Append('|'); sb.Append((long)Math.Round(m[j] * 1e6, MidpointRounding.ToEven)); }
                                    for (int j = 9; j < 12; j++) { sb.Append('|'); sb.Append((long)Math.Round(m[j] * 1e3, MidpointRounding.ToEven)); }
                                    string mKey = sb.ToString();
                                    if (!mergedIndex.TryGetValue(mKey, out int idx))
                                    {
                                        mergedIndex[mKey] = mergedOrder.Count;
                                        mergedOrder.Add((ag.Key, 1 << level, ag.Steps[i], m, ag.Kind));
                                    }
                                    else
                                    {
                                        var cur = mergedOrder[idx];
                                        mergedOrder[idx] = (cur.Key, cur.Mask | (1 << level), Math.Min(cur.Step, ag.Steps[i]), cur.Mat, cur.Kind);
                                    }
                                }
                            }
                        }
                    }
                }

                int dropped = 0, mCount = 0, lCount = 0;
                foreach (var entry in mergedOrder)
                {
                    if ((entry.Mask & (1 << n.Level)) == 0) { dropped++; continue; }
                    if (!assetIds.ContainsKey(entry.Key)) assetIds[entry.Key] = entry.Kind;
                    rows.Add((n.Guid, entry.Key, entry.Kind, entry.Mask, entry.Step, entry.Mat));
                    if (entry.Kind == "M") mCount++;
                    else if (entry.Kind == "L") lCount++;
                }
                summaryLines.Add($"  {n.Name} at level {n.Level}: {mCount} static mesh rows, {lCount} level instance rows ({dropped} more stand at other levels only)");
            }

            var assets = assetIds.OrderBy(kv => kv.Value, StringComparer.Ordinal).ThenBy(kv => kv.Key, StringComparer.Ordinal).ToList();
            var aid = new Dictionary<string, int>(assets.Count);
            for (int i = 0; i < assets.Count; i++) aid[assets[i].Key] = i;

            var sortedRows = rows
                .Select((r, idx) => (Row: r, Idx: idx))
                .OrderBy(x => x.Row.Guid)
                .ThenBy(x => aid[x.Row.Key])
                .ThenBy(x => x.Row.Mask)
                .ThenBy(x => Math.Round(x.Row.Mat[0], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[1], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[2], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[3], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[4], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[5], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[6], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[7], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[8], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[9], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[10], 3, MidpointRounding.ToEven))
                .ThenBy(x => Math.Round(x.Row.Mat[11], 3, MidpointRounding.ToEven))
                .ThenBy(x => x.Idx)
                .Select(x => x.Row)
                .ToList();

            var outSb = new StringBuilder(6 * 1024 * 1024);
            outSb.Append("# town_layout_items v1: the placed static meshes (M) and level instances (L) of the five towns' layouts, each node at the level it starts at (tools/capture/gen_town_layouts.py)\n");
            outSb.Append("# node levels: ").Append(string.Join(", ", nodes.Select(n => $"{n.Name} {n.Level}"))).Append('\n');
            outSb.Append("# columns of a row: node, asset, levels (hex, bit n = node level n), stepup (0: a character cannot step up onto it), m00 m01 m02 m10 m11 m12 m20 m21 m22 tx ty tz (world cm)\n");
            foreach (var n in nodes)
            {
                outSb.Append($"@node\t{n.Guid}\t{n.Name}\t{n.Layout}\t{n.Type}\t{FormatNum(n.X, 6)}\t{FormatNum(n.Y, 6)}\t{FormatNum(n.Z, 6)}\t{FormatNum(n.Yaw, 9)}\n");
            }
            for (int i = 0; i < assets.Count; i++)
            {
                outSb.Append($"@asset\t{i}\t{assets[i].Value}\t{assets[i].Key}\n");
            }
            foreach (var r in sortedRows)
            {
                outSb.Append(r.Guid).Append('\t').Append(aid[r.Key]).Append('\t').Append(r.Mask.ToString("x", CultureInfo.InvariantCulture)).Append('\t').Append(r.Step);
                for (int j = 0; j < 9; j++) outSb.Append('\t').Append(FormatRotNum(r.Mat[j]));
                for (int j = 9; j < 12; j++) outSb.Append('\t').Append(FormatNum(r.Mat[j], 3));
                outSb.Append('\n');
            }
            string finalText = outSb.ToString();
            int byteCount = Encoding.UTF8.GetByteCount(finalText);
            summaryLines.Add($"gen_town_layouts: {assets.Count} assets, {sortedRows.Count} rows, {byteCount} bytes");
            return (finalText, summaryLines);
        }
    }
}
'@

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$opts = Parse-CliOptions -RepoRoot $repoRoot -CliArgs $args
Initialize-ManagedSqlite -RepoRoot $repoRoot
Initialize-TownLayoutEngine

$builder = [SandalCore.TownLayouts.TownLayoutBuilder]::new()
$res = $builder.Build($opts.ContentDb, $opts.Seed)
$text = $res.Item1
$summaryLines = $res.Item2
foreach ($line in $summaryLines) { Write-Output $line }

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$data = $utf8NoBom.GetBytes($text)
$fullOut = [System.IO.Path]::GetFullPath($opts.Out)
if ($opts.Check) {
    if (-not (Test-Path -LiteralPath $fullOut)) {
        Write-Output "gen_town_layouts: $($opts.Out) is missing"
        exit 1
    }
    $have = [System.IO.File]::ReadAllBytes($fullOut)
    $same = ($have.Length -eq $data.Length) -and [System.Linq.Enumerable]::SequenceEqual($have, $data)
    Write-Output "gen_town_layouts: $($opts.Out) $($same ? 'identical' : 'DIFFERS')"
    exit ($same ? 0 : 1)
}
$outDir = Split-Path -Parent $fullOut
if ($outDir) { [void][System.IO.Directory]::CreateDirectory($outDir) }
$tmpPath = "$fullOut.tmp"
[System.IO.File]::WriteAllBytes($tmpPath, $data)
Move-Item -Force -LiteralPath $tmpPath -Destination $fullOut
Write-Output "gen_town_layouts: wrote $($opts.Out)"
