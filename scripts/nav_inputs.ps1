#Requires -Version 7.2
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Format-NavInputRow {
    param([object[]]$Fields)
    $inv = [System.Globalization.CultureInfo]::InvariantCulture
    $guidText = [System.Convert]::ToString($Fields[0], $inv)
    $coords = [System.Collections.Generic.List[string]]::new(4)
    for ($i = 1; $i -lt 5; $i++) {
        $val = $Fields[$i]
        $num = 0.0
        if ($val -is [double] -or $val -is [float] -or $val -is [decimal]) {
            $coords.Add(([double]$val).ToString($inv))
        } elseif ([double]::TryParse([System.Convert]::ToString($val, $inv), [System.Globalization.NumberStyles]::Float, $inv, [ref]$num)) {
            $coords.Add($num.ToString($inv))
        } else {
            throw "Invalid numeric coordinate '$val' in city_node row."
        }
    }
    return "$guidText`t$($coords -join "`t")"
}

function Read-NavRowsWithSqliteCli {
    param([string]$SqliteExe, [string]$DbPath)
    $fullPath = [System.IO.Path]::GetFullPath($DbPath)
    $sql = 'SELECT guid, x, y, z, yaw FROM city_node ORDER BY guid;'
    $raw = @(& $SqliteExe -readonly -batch -noheader -separator "`t" $fullPath $sql 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw (($raw | ForEach-Object { "$_" }) -join ' ').Trim()
    }
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $raw) {
        $text = "$line".TrimEnd("`r", "`n")
        if ([string]::IsNullOrWhiteSpace($text)) { continue }
        $parts = $text -split "`t"
        if ($parts.Count -lt 5) { throw "Unexpected sqlite3 column count in '$text'." }
        $rows.Add((Format-NavInputRow -Fields $parts[0..4]))
    }
    return $rows.ToArray()
}

function Initialize-ManagedSqlite {
    if ('Microsoft.Data.Sqlite.SqliteConnection' -as [type]) { return $true }
    $repoRoot = Split-Path $PSScriptRoot -Parent
    $rid = if ($IsWindows) { 'win-x64' } elseif ($IsLinux) { 'linux-x64' } else { 'osx-x64' }
    $binDir = Get-ChildItem -Path (Join-Path $repoRoot 'build/dotnet/bin') -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($binDir) {
        $dir = $binDir.DirectoryName
        $native = Get-ChildItem -Path (Join-Path $dir "runtimes/$rid/native") -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $native) { $native = Get-ChildItem -Path $dir -Recurse -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Select-Object -First 1 }
        if ($native) { [void][System.Runtime.InteropServices.NativeLibrary]::Load($native.FullName) }
        foreach ($dll in @('SQLitePCLRaw.core.dll', 'SQLitePCLRaw.provider.e_sqlite3.dll', 'SQLitePCLRaw.batteries_v2.dll', 'Microsoft.Data.Sqlite.dll')) {
            $p = Join-Path $dir $dll
            if (Test-Path -LiteralPath $p) { Add-Type -LiteralPath $p }
        }
        [SQLitePCL.Batteries_V2]::Init()
        return $true
    }
    $nugetCandidates = @($env:NUGET_PACKAGES, (Join-Path $HOME '.nuget/packages'), (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.nuget/packages')) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    $nuget = $nugetCandidates | Select-Object -First 1
    if (-not $nuget) { return $false }
    $nativePkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.lib.e_sqlite3') -Recurse -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match $rid } | Select-Object -Last 1
    $corePkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.core') -Recurse -Filter 'SQLitePCLRaw.core.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $provPkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.provider.e_sqlite3') -Recurse -Filter 'SQLitePCLRaw.provider.e_sqlite3.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $batPkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.bundle_e_sqlite3') -Recurse -Filter 'SQLitePCLRaw.batteries_v2.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $mdsPkg = Get-ChildItem -Path (Join-Path $nuget 'microsoft.data.sqlite.core/8.*') -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'net8\.0' } | Select-Object -Last 1
    if ($nativePkg -and $corePkg -and $provPkg -and $batPkg -and $mdsPkg) {
        [void][System.Runtime.InteropServices.NativeLibrary]::Load($nativePkg.FullName)
        foreach ($dll in @($corePkg.FullName, $provPkg.FullName, $batPkg.FullName, $mdsPkg.FullName)) { Add-Type -LiteralPath $dll }
        [SQLitePCL.Batteries_V2]::Init()
        return $true
    }
    return $false
}

function Read-NavRowsWithManagedSqlite {
    param([string]$DbPath)
    $cb = [Microsoft.Data.Sqlite.SqliteConnectionStringBuilder]::new()
    $cb.DataSource = [System.IO.Path]::GetFullPath($DbPath)
    $cb.Mode = [Microsoft.Data.Sqlite.SqliteOpenMode]::ReadOnly
    $conn = [Microsoft.Data.Sqlite.SqliteConnection]::new($cb.ToString())
    try {
        $conn.Open()
        $cmd = $conn.CreateCommand()
        $cmd.CommandText = 'SELECT guid, x, y, z, yaw FROM city_node ORDER BY guid'
        $reader = $cmd.ExecuteReader()
        $rows = [System.Collections.Generic.List[string]]::new()
        while ($reader.Read()) {
            $rows.Add((Format-NavInputRow -Fields @($reader.GetValue(0), $reader.GetDouble(1), $reader.GetDouble(2), $reader.GetDouble(3), $reader.GetDouble(4))))
        }
        return $rows.ToArray()
    } finally {
        $conn.Dispose()
    }
}

function Read-NavRowsWithPythonFallback {
    param([string]$DbPath)
    $pyCmd = $null
    foreach ($candidate in @('python3', 'python', 'py')) {
        $found = Get-Command $candidate -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $pyCmd = $found.Source; break }
    }
    if (-not $pyCmd -and $env:LOCALAPPDATA) {
        $winPy = Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA 'Programs/Python/Python*/python.exe') -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($winPy) { $pyCmd = $winPy.FullName }
    }
    if (-not $pyCmd -and (Test-Path -LiteralPath '/usr/bin/python3')) { $pyCmd = '/usr/bin/python3' }
    if (-not $pyCmd) { throw 'Neither sqlite3 CLI, Microsoft.Data.Sqlite, nor python3 is available.' }
    $pyScript = 'import pathlib,sqlite3,sys; p=pathlib.Path(sys.argv[1]).resolve(); c=sqlite3.connect("file:"+str(p).replace("\\","/")+"?mode=ro",uri=True); [sys.stdout.write("\t".join(str(v) for v in r)+"\n") for r in c.execute("SELECT guid, x, y, z, yaw FROM city_node ORDER BY guid")]; c.close()'
    $pyArgs = if ([System.IO.Path]::GetFileNameWithoutExtension($pyCmd) -eq 'py') { @('-3', '-I', '-c', $pyScript, $DbPath) } else { @('-I', '-c', $pyScript, $DbPath) }
    $raw = @(& $pyCmd @pyArgs 2>&1)
    if ($LASTEXITCODE -ne 0) { throw (($raw | ForEach-Object { "$_" }) -join ' ').Trim() }
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $raw) {
        $text = "$line".TrimEnd("`r", "`n")
        if ([string]::IsNullOrWhiteSpace($text)) { continue }
        $parts = $text -split "`t"
        $rows.Add((Format-NavInputRow -Fields $parts[0..4]))
    }
    return $rows.ToArray()
}

if ($args.Count -ne 1 -or [string]::IsNullOrWhiteSpace("$($args[0])")) {
    [Console]::Error.WriteLine('usage: nav_inputs.ps1 CONTENT_DB')
    exit 2
}
$dbPath = [string]$args[0]
if (-not (Test-Path -LiteralPath $dbPath -PathType Leaf)) {
    [Console]::Error.WriteLine("nav_inputs: $dbPath is missing")
    exit 1
}
try {
    $fileInfo = Get-Item -LiteralPath $dbPath
    if ($fileInfo.Length -lt 100) { throw 'file is not a valid SQLite database' }
    $sqliteCli = if ($env:SANDAL_DISABLE_SQLITE3_CLI -eq '1') { $null } else { Get-Command sqlite3 -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1 }
    $lines = if ($null -ne $sqliteCli) {
        @(Read-NavRowsWithSqliteCli -SqliteExe $sqliteCli.Source -DbPath $dbPath)
    } elseif (Initialize-ManagedSqlite) {
        @(Read-NavRowsWithManagedSqlite -DbPath $dbPath)
    } else {
        @(Read-NavRowsWithPythonFallback -DbPath $dbPath)
    }
    foreach ($outLine in $lines) {
        [Console]::Out.WriteLine($outLine)
    }
    exit 0
} catch {
    [Console]::Error.WriteLine("nav_inputs: ${dbPath}: city_node cannot be read ($($_.Exception.Message))")
    exit 1
}
