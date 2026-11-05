#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($null -ne (Get-Module Pester) -and (Get-Module Pester).Version.Major -lt 5) {
    function Should {
        $mapped = [System.Collections.Generic.List[object]]::new()
        foreach ($a in $args) {
            if ($a -is [string] -and $a -eq '-BeTrue') {
                $mapped.AddRange([object[]]@('Be', $true))
            } elseif ($a -is [string] -and $a -eq '-BeFalse') {
                $mapped.AddRange([object[]]@('Be', $false))
            } elseif ($a -is [string] -and $a.StartsWith('-')) {
                $mapped.Add($a.Substring(1))
            } else {
                $mapped.Add($a)
            }
        }
        $input | Pester\Should @mapped
    }
}

Describe 'scripts/nav_inputs.ps1 1:1 City Node Extraction & Stamp Input Contract' {
    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
        $script:NavInputsScript = Join-Path $script:RepoRoot 'scripts\nav_inputs.ps1'

        if (-not ('SandalNavTestSqlite' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SandalNavTestSqlite {
    [DllImport("winsqlite3.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
    private static extern int sqlite3_open(string filename, out IntPtr db);

    [DllImport("winsqlite3.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
    private static extern int sqlite3_exec(IntPtr db, string sql, IntPtr callback, IntPtr arg, out IntPtr errmsg);

    [DllImport("winsqlite3.dll", CallingConvention = CallingConvention.StdCall)]
    private static extern int sqlite3_close(IntPtr db);

    public static void Execute(string dbPath, string sql) {
        IntPtr db;
        int rc = sqlite3_open(dbPath, out db);
        if (rc != 0) throw new InvalidOperationException("sqlite3_open failed: " + rc);
        try {
            IntPtr err;
            rc = sqlite3_exec(db, sql, IntPtr.Zero, IntPtr.Zero, out err);
            if (rc != 0) {
                string msg = err != IntPtr.Zero ? Marshal.PtrToStringAnsi(err) : ("rc=" + rc);
                throw new InvalidOperationException("sqlite3_exec failed: " + msg);
            }
        } finally {
            sqlite3_close(db);
        }
    }
}
'@
        }

        function Invoke-SqliteNonQuery {
            param(
                [Parameter(Mandatory)][string]$DbPath,
                [Parameter(Mandatory)][string]$Sql
            )
            [SandalNavTestSqlite]::Execute($DbPath, $Sql)
        }

        function New-CityNodeDatabase {
            param([Parameter(Mandatory)][string]$DbPath)
            $sql = @"
CREATE TABLE city_node (guid INTEGER PRIMARY KEY, name TEXT, x REAL, y REAL, z REAL, yaw REAL);
CREATE TABLE abilities (id INTEGER PRIMARY KEY, power INTEGER);
INSERT INTO city_node (guid, name, x, y, z, yaw) VALUES
    (20, 'Winstead', 100.5, -200.25, 15.0, 90.0),
    (10, 'Lionhold', -50.125, 300.75, 8.5, 180.0),
    (30, 'Halcyon', 0.0, 42.0, -3.25, 270.5);
INSERT INTO abilities (id, power) VALUES (1, 50);
"@
            Invoke-SqliteNonQuery -DbPath $DbPath -Sql $sql
        }

        function Invoke-NavInputsProcess {
            param([string[]]$Arguments = @())
            $psi = [System.Diagnostics.ProcessStartInfo]::new()
            $psi.FileName = (Get-Process -Id $PID).Path
            $psi.ArgumentList.Add('-NoProfile')
            $psi.ArgumentList.Add('-File')
            $psi.ArgumentList.Add($script:NavInputsScript)
            foreach ($arg in $Arguments) {
                $psi.ArgumentList.Add($arg)
            }
            $psi.RedirectStandardOutput = $true
            $psi.RedirectStandardError = $true
            $psi.UseShellExecute = $false
            $psi.CreateNoWindow = $true
            $proc = [System.Diagnostics.Process]::Start($psi)
            $stdout = $proc.StandardOutput.ReadToEnd()
            $stderr = $proc.StandardError.ReadToEnd()
            $proc.WaitForExit()
            return [pscustomobject]@{
                ExitCode = $proc.ExitCode
                StdOut   = $stdout
                StdErr   = $stderr
            }
        }

        function Get-TextSha256 {
            param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
            return [System.Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
        }
    }

    It 'emits city_node rows ordered by guid with exit code 0' {
        $tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-navinputs-' + [guid]::NewGuid().ToString('N'))
        $null = New-Item -ItemType Directory -Path $tmpDir -Force
        try {
            $dbPath = Join-Path $tmpDir 'content.db'
            New-CityNodeDatabase -DbPath $dbPath

            $res = Invoke-NavInputsProcess -Arguments @($dbPath)
            $res.ExitCode | Should -Be 0
            $res.StdErr.Trim() | Should -Be ''

            $lines = @($res.StdOut -split '\r?\n' | Where-Object { $_ -ne '' })
            $lines.Count | Should -Be 3
            $lines[0] | Should -Match '^10[\t ]-50\.125[\t ]300\.75[\t ]8\.5[\t ]180(?:\.0)?$'
            $lines[1] | Should -Match '^20[\t ]100\.5[\t ]-200\.25[\t ]15(?:\.0)?[\t ]90(?:\.0)?$'
            $lines[2] | Should -Match '^30[\t ]0(?:\.0)?[\t ]42(?:\.0)?[\t ]-3\.25[\t ]270\.5$'
        } finally {
            if (Test-Path -LiteralPath $tmpDir) {
                Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }

    It 'produces identical output and SHA-256 across non-node mutations and changes when city_node pose changes' {
        $tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-navinputs-hash-' + [guid]::NewGuid().ToString('N'))
        $null = New-Item -ItemType Directory -Path $tmpDir -Force
        try {
            $dbPath = Join-Path $tmpDir 'content.db'
            New-CityNodeDatabase -DbPath $dbPath

            $baseRes = Invoke-NavInputsProcess -Arguments @($dbPath)
            $baseRes.ExitCode | Should -Be 0
            $baseSha = Get-TextSha256 -Text $baseRes.StdOut

            Invoke-SqliteNonQuery -DbPath $dbPath -Sql "UPDATE abilities SET power = 999 WHERE id = 1; UPDATE city_node SET name = 'Renamed' WHERE guid = 10;"
            $sameRes = Invoke-NavInputsProcess -Arguments @($dbPath)
            $sameRes.ExitCode | Should -Be 0
            $sameRes.StdOut | Should -Be $baseRes.StdOut
            (Get-TextSha256 -Text $sameRes.StdOut) | Should -Be $baseSha

            Invoke-SqliteNonQuery -DbPath $dbPath -Sql "UPDATE city_node SET yaw = 45.0 WHERE guid = 10;"
            $changedRes = Invoke-NavInputsProcess -Arguments @($dbPath)
            $changedRes.ExitCode | Should -Be 0
            ($changedRes.StdOut -ne $baseRes.StdOut) | Should -BeTrue
            ((Get-TextSha256 -Text $changedRes.StdOut) -ne $baseSha) | Should -BeTrue
        } finally {
            if (Test-Path -LiteralPath $tmpDir) {
                Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }

    It 'returns exit code 2 on missing or extra arguments' {
        $noneRes = Invoke-NavInputsProcess -Arguments @()
        $noneRes.ExitCode | Should -Be 2
        $noneRes.StdOut | Should -Be ''
        $noneRes.StdErr | Should -Match '(?i)usage:.*nav_inputs'

        $extraRes = Invoke-NavInputsProcess -Arguments @('a.db', 'b.db')
        $extraRes.ExitCode | Should -Be 2
        $extraRes.StdOut | Should -Be ''
        $extraRes.StdErr | Should -Match '(?i)usage:.*nav_inputs'
    }

    It 'returns exit code 1 when path is missing or is a directory' {
        $tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-navinputs-missing-' + [guid]::NewGuid().ToString('N'))
        $null = New-Item -ItemType Directory -Path $tmpDir -Force
        try {
            $missingDb = Join-Path $tmpDir 'missing_content.db'
            $missingRes = Invoke-NavInputsProcess -Arguments @($missingDb)
            $missingRes.ExitCode | Should -Be 1
            $missingRes.StdOut | Should -Be ''
            $missingRes.StdErr | Should -Match '(?i)missing|not found|does not exist'

            $dirRes = Invoke-NavInputsProcess -Arguments @($tmpDir)
            $dirRes.ExitCode | Should -Be 1
            $dirRes.StdOut | Should -Be ''
            $dirRes.StdErr | Should -Match '(?i)missing|not found|directory'
        } finally {
            if (Test-Path -LiteralPath $tmpDir) {
                Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }

    It 'returns exit code 1 when city_node table is missing or database file is corrupt' {
        $tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-navinputs-corrupt-' + [guid]::NewGuid().ToString('N'))
        $null = New-Item -ItemType Directory -Path $tmpDir -Force
        try {
            $emptyDb = Join-Path $tmpDir 'empty.db'
            Invoke-SqliteNonQuery -DbPath $emptyDb -Sql 'CREATE TABLE unrelated (id INTEGER PRIMARY KEY);'

            $noTableRes = Invoke-NavInputsProcess -Arguments @($emptyDb)
            $noTableRes.ExitCode | Should -Be 1
            $noTableRes.StdOut | Should -Be ''
            $noTableRes.StdErr | Should -Match '(?i)city_node|cannot be read|error'

            $corruptDb = Join-Path $tmpDir 'corrupt.db'
            [System.IO.File]::WriteAllText($corruptDb, 'not a valid sqlite3 file', [System.Text.UTF8Encoding]::new($false))

            $corruptRes = Invoke-NavInputsProcess -Arguments @($corruptDb)
            $corruptRes.ExitCode | Should -Be 1
            $corruptRes.StdOut | Should -Be ''
            $corruptRes.StdErr | Should -Match '(?i)city_node|cannot be read|corrupt|not a database|error'
        } finally {
            if (Test-Path -LiteralPath $tmpDir) {
                Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }
}
