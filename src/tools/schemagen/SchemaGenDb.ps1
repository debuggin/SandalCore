#Requires -Version 7.0
# SPDX-License-Identifier: Apache-2.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$script:SqliteLoaded = $false

function Resolve-SqliteBinDir {
    [CmdletBinding()]
    param()
    $candidates = @(
        (Join-Path $script:RepoRoot 'build\dotnet\bin\AocExtractor\Release\net10.0')
        (Join-Path $script:RepoRoot 'build\dotnet\bin\AocExtractor\Debug\net10.0')
        (Join-Path $script:RepoRoot 'src\tools\aoc-extractor\bin\Release\net10.0')
        (Join-Path $script:RepoRoot 'src\tools\aoc-extractor\bin\Debug\net10.0')
    )
    foreach ($dir in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $dir 'Microsoft.Data.Sqlite.dll')) {
            return $dir
        }
    }
    $binRoot = Join-Path $script:RepoRoot 'build\dotnet\bin'
    if (Test-Path -LiteralPath $binRoot -PathType Container) {
        $found = Get-ChildItem -Path $binRoot -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $found) { return $found.DirectoryName }
    }
    return $null
}

function Initialize-SchemaGenSqliteFromNuGet {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$NugetRoot)
    $rid = if ($IsWindows) { 'win-x64' } elseif ($IsLinux) { 'linux-x64' } else { 'osx-x64' }
    $nativePkg = Get-ChildItem -Path (Join-Path $NugetRoot 'sqlitepclraw.lib.e_sqlite3') -Recurse -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match $rid } | Select-Object -Last 1
    $corePkg = Get-ChildItem -Path (Join-Path $NugetRoot 'sqlitepclraw.core') -Recurse -Filter 'SQLitePCLRaw.core.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'net10\.0|net9\.0|net8\.0|netstandard2\.0' } | Select-Object -Last 1
    $provPkg = Get-ChildItem -Path (Join-Path $NugetRoot 'sqlitepclraw.provider.e_sqlite3') -Recurse -Filter 'SQLitePCLRaw.provider.e_sqlite3.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'net10\.0|net9\.0|net8\.0|net6\.0|netstandard2\.0' } | Select-Object -Last 1
    $batPkg = Get-ChildItem -Path (Join-Path $NugetRoot 'sqlitepclraw.bundle_e_sqlite3') -Recurse -Filter 'SQLitePCLRaw.batteries_v2.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'net10\.0|net9\.0|net8\.0|net6\.0|netstandard2\.0' } | Select-Object -Last 1
    $mdsPkg = Get-ChildItem -Path (Join-Path $NugetRoot 'microsoft.data.sqlite.core') -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'net10\.0|net9\.0|net8\.0|netstandard2\.0' } | Select-Object -Last 1
    if ($nativePkg -and $corePkg -and $provPkg -and $batPkg -and $mdsPkg) {
        [void][System.Runtime.InteropServices.NativeLibrary]::Load($nativePkg.FullName)
        foreach ($dll in @($corePkg.FullName, $provPkg.FullName, $batPkg.FullName, $mdsPkg.FullName)) {
            [void][System.Reflection.Assembly]::LoadFrom($dll)
        }
        [SQLitePCL.Batteries_V2]::Init()
        return $true
    }
    return $false
}

function Initialize-SchemaGenSqlite {
    [CmdletBinding()]
    param()
    if ($script:SqliteLoaded) { return }
    $binDir = Resolve-SqliteBinDir
    if ($null -ne $binDir) {
        $nativeDll = Join-Path $binDir 'runtimes\win-x64\native\e_sqlite3.dll'
        if (Test-Path -LiteralPath $nativeDll) {
            [void][System.Runtime.InteropServices.NativeLibrary]::Load($nativeDll)
        }
        foreach ($dll in @('SQLitePCLRaw.core.dll', 'SQLitePCLRaw.provider.e_sqlite3.dll', 'SQLitePCLRaw.batteries_v2.dll', 'Microsoft.Data.Sqlite.dll')) {
            $p = Join-Path $binDir $dll
            if (Test-Path -LiteralPath $p) { [void][System.Reflection.Assembly]::LoadFrom($p) }
        }
        [SQLitePCL.Batteries_V2]::Init()
        $script:SqliteLoaded = $true
        return
    }
    $nugetRoot = Join-Path $env:USERPROFILE '.nuget\packages'
    if ((Test-Path -LiteralPath $nugetRoot) -and (Initialize-SchemaGenSqliteFromNuGet -NugetRoot $nugetRoot)) {
        $script:SqliteLoaded = $true
        return
    }
    throw 'Microsoft.Data.Sqlite assembly not found in build/dotnet/bin or NuGet cache.'
}

function Read-SqliteReaderRows {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Reader)
    $rows = [System.Collections.Generic.List[hashtable]]::new()
    while ($Reader.Read()) {
        $row = [ordered]@{}
        for ($i = 0; $i -lt $Reader.FieldCount; $i++) {
            $row[$Reader.GetName($i)] = if ($Reader.IsDBNull($i)) { $null } else { $Reader.GetValue($i) }
        }
        $rows.Add($row)
    }
    return ,$rows.ToArray()
}

function Invoke-SchemaGenSqliteQuery {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$DbPath,
        [Parameter(Mandatory)][string]$Query
    )
    if (-not (Test-Path -LiteralPath $DbPath)) {
        throw "SQLite DB not found: $DbPath"
    }
    $sqliteCli = Get-Command 'sqlite3' -ErrorAction SilentlyContinue
    if ($null -ne $sqliteCli -and -not $script:SqliteLoaded) {
        $raw = & $sqliteCli.Source -json $DbPath $Query
        if ($LASTEXITCODE -eq 0) {
            if ([string]::IsNullOrWhiteSpace($raw)) { return @() }
            return @(ConvertFrom-Json -InputObject ($raw -join "`n") -AsHashtable)
        }
    }
    Initialize-SchemaGenSqlite
    $resolved = (Resolve-Path -LiteralPath $DbPath).Path
    $conn = [Microsoft.Data.Sqlite.SqliteConnection]::new("Data Source=$resolved;Mode=ReadOnly")
    $conn.Open()
    try {
        $cmd = $conn.CreateCommand()
        $cmd.CommandText = $Query
        $reader = $cmd.ExecuteReader()
        try {
            return (Read-SqliteReaderRows -Reader $reader)
        } finally {
            $reader.Dispose()
        }
    } finally {
        $conn.Dispose()
    }
}

function ConvertFrom-SchemaGenGzipJson {
    [CmdletBinding()]
    param([Parameter(Mandatory)][byte[]]$CompressedBytes)
    $ms = [System.IO.MemoryStream]::new($CompressedBytes)
    try {
        $gz = [System.IO.Compression.GZipStream]::new($ms, [System.IO.Compression.CompressionMode]::Decompress)
        try {
            $sr = [System.IO.StreamReader]::new($gz, [System.Text.Encoding]::UTF8)
            try {
                return (ConvertFrom-Json -InputObject $sr.ReadToEnd() -AsHashtable)
            } finally {
                $sr.Dispose()
            }
        } finally {
            $gz.Dispose()
        }
    } finally {
        $ms.Dispose()
    }
}
