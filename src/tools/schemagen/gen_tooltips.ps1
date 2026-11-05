#!/usr/bin/env pwsh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

[CmdletBinding()]
param(
    [string]$ContentDb = '',
    [string]$Out = '',
    [string]$Header = '',
    [switch]$Check,
    [int]$Level = 25,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
Import-Module (Join-Path $PSScriptRoot 'TooltipEngine.psm1') -Force

function Resolve-CliOptions {
    param(
        [string]$InitDb,
        [string]$InitOut,
        [string]$InitHeader,
        [bool]$InitCheck,
        [string[]]$ExtraArgs
    )
    $opts = @{
        Db     = $InitDb
        Out    = $InitOut
        Header = $InitHeader
        Check  = $InitCheck
        Level  = $script:Level
    }
    $i = 0
    $list = @($ExtraArgs | Where-Object { $null -ne $_ })
    while ($i -lt $list.Count) {
        $arg = $list[$i]
        switch ($arg) {
            { $_ -in @('--db', '--content-db', '-Db', '-db') } { $i++; $opts.Db = $list[$i] }
            { $_ -in @('--out', '-Out', '-out') }              { $i++; $opts.Out = $list[$i] }
            { $_ -in @('--header', '-Header', '-header') }     { $i++; $opts.Header = $list[$i] }
            { $_ -in @('--check', '-Check', '-check') }        { $opts.Check = $true }
            { $_ -in @('--level', '-Level', '-level') }        { $i++; $opts.Level = [int]$list[$i] }
        }
        $i++
    }
    if (-not $opts.Db) {
        $opts.Db = Join-Path $script:RepoRoot 'dist\content\content.db'
    }
    if (-not $opts.Header) {
        $opts.Header = if ($opts.Out) { $opts.Out } else { Join-Path $script:RepoRoot 'build\gen\world\tests\tooltip_key.h' }
    }
    return $opts
}

function Sync-TooltipOutputFile {
    param(
        [string]$FilePath,
        [string]$Text,
        [bool]$CheckOnly
    )
    $oldBytes = [byte[]]@()
    if (Test-Path -LiteralPath $FilePath -PathType Leaf) {
        $oldBytes = [System.IO.File]::ReadAllBytes($FilePath)
    }
    $lfCount = 0
    $crlfCount = 0
    for ($i = 0; $i -lt $oldBytes.Length; $i++) {
        if ($oldBytes[$i] -eq 10) {
            $lfCount++
            if ($i -gt 0 -and $oldBytes[$i - 1] -eq 13) { $crlfCount++ }
        }
    }
    $useCrlf = ($lfCount -gt 0) -and (($crlfCount * 2) -gt $lfCount)
    $normalized = $Text.Replace("`r`n", "`n")
    if ($useCrlf) { $normalized = $normalized.Replace("`n", "`r`n") }
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    $newBytes = $utf8NoBom.GetBytes($normalized)
    $rel = [System.IO.Path]::GetRelativePath($script:RepoRoot, $FilePath).Replace('\', '/')
    if ($oldBytes.Length -eq $newBytes.Length -and [System.Linq.Enumerable]::SequenceEqual($oldBytes, $newBytes)) {
        Write-Host "${rel}: unchanged ($($newBytes.Length) bytes)"
        return $true
    }
    if ($CheckOnly) {
        Write-Host "${rel}: DIFFERS from the regenerated text ($($oldBytes.Length) bytes on disk, $($newBytes.Length) regenerated)"
        return $false
    }
    $parent = Split-Path -Parent $FilePath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    [System.IO.File]::WriteAllBytes($FilePath, $newBytes)
    Write-Host "${rel}: updated ($($newBytes.Length) bytes)"
    return $false
}

$opts = Resolve-CliOptions -InitDb $ContentDb -InitOut $Out -InitHeader $Header -InitCheck $Check.IsPresent -ExtraArgs $RemainingArgs
if (-not (Test-Path -LiteralPath $opts.Db -PathType Leaf)) {
    Write-Error "no content.db at $($opts.Db) (cd services && go run ./cmd/contentc)"
    exit 1
}

$built = Invoke-TooltipHeaderBuild -DbPath $opts.Db -RepoRoot $script:RepoRoot -Level $opts.Level
$ok = Sync-TooltipOutputFile -FilePath $opts.Header -Text $built.HeaderText -CheckOnly $opts.Check
Write-Host "$($built.AbilityCount) abilities in $($built.GroupCount) groups"
if ($opts.Check -and -not $ok) {
    exit 1
}
exit 0
