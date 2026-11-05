#Requires -Version 7.2
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

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

Describe 'Wave 26Q: Python-vs-PowerShell Generator Data Parity & Dynamic Gate-Sync Suite' {
    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
        $script:ContentDb = Join-Path $script:RepoRoot 'dist\content\content.db'
        $script:ZoiChunks = Join-Path $script:RepoRoot 'dist\content\zoi_chunks'
        $script:HasContentAssets = (Test-Path -LiteralPath $script:ContentDb -PathType Leaf) -and (Test-Path -LiteralPath $script:ZoiChunks -PathType Container)

        function Get-ArtifactMetrics {
            param([Parameter(Mandatory)][string]$FilePath)
            $bytes = [System.IO.File]::ReadAllBytes($FilePath)
            $sha = [System.Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
            $text = [System.Text.Encoding]::UTF8.GetString($bytes)
            $lfLines = ($text -split "`n").Count - 1
            return [pscustomobject]@{
                Bytes   = $bytes.Length
                Sha256  = $sha
                LfLines = $lfLines
            }
        }

        function Assert-GoldenArtifact {
            param(
                [Parameter(Mandatory)][string]$FilePath,
                [Parameter(Mandatory)][int]$ExpectedBytes,
                [Parameter(Mandatory)][string[]]$AllowedSha256,
                [int]$ExpectedLfLines = -1
            )
            (Test-Path -LiteralPath $FilePath -PathType Leaf) | Should -BeTrue
            $metrics = Get-ArtifactMetrics -FilePath $FilePath
            $metrics.Bytes | Should -Be $ExpectedBytes
            ($AllowedSha256 -contains $metrics.Sha256) | Should -BeTrue
            if ($ExpectedLfLines -ge 0) {
                $metrics.LfLines | Should -Be $ExpectedLfLines
            }
        }

        function Invoke-GeneratorCommand {
            param(
                [Parameter(Mandatory)][string]$Executable,
                [string[]]$Arguments = @()
            )
            $psi = [System.Diagnostics.ProcessStartInfo]::new()
            $psi.FileName = $Executable
            foreach ($arg in $Arguments) {
                $psi.ArgumentList.Add($arg)
            }
            $psi.WorkingDirectory = $script:RepoRoot
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

        function Invoke-PwshGenerator {
            param(
                [Parameter(Mandatory)][string]$RelScriptPath,
                [string[]]$Arguments = @()
            )
            $fullScript = Join-Path $script:RepoRoot $RelScriptPath
            $pwshExe = (Get-Process -Id $PID).Path
            $allArgs = @('-NoProfile', '-File', $fullScript) + $Arguments
            return Invoke-GeneratorCommand -Executable $pwshExe -Arguments $allArgs
        }

        function Resolve-PythonCommand {
            $py = Get-Command python -ErrorAction SilentlyContinue
            if ($null -ne $py) {
                $probe = Invoke-GeneratorCommand -Executable $py.Source -Arguments @('--version')
                if ($probe.ExitCode -eq 0) {
                    return [pscustomobject]@{ Available = $true; Exe = $py.Source; PrefixArgs = [string[]]@() }
                }
            }
            $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
            if ($null -ne $wsl) {
                $probe = Invoke-GeneratorCommand -Executable $wsl.Source -Arguments @('python3', '--version')
                if ($probe.ExitCode -eq 0) {
                    return [pscustomobject]@{ Available = $true; Exe = $wsl.Source; PrefixArgs = [string[]]@('python3') }
                }
            }
            return [pscustomobject]@{ Available = $false; Exe = ''; PrefixArgs = [string[]]@() }
        }

        function Resolve-PosixShExecutable {
            $gitSh = 'C:\Program Files\Git\bin\sh.exe'
            if (Test-Path -LiteralPath $gitSh -PathType Leaf) { return $gitSh }
            $shCmd = Get-Command sh -ErrorAction SilentlyContinue
            if ($null -ne $shCmd) { return $shCmd.Source }
            $bashCmd = Get-Command bash -ErrorAction SilentlyContinue
            if ($null -ne $bashCmd) { return $bashCmd.Source }
            return ''
        }

        function Get-PythonGateSourceState {
            param([Parameter(Mandatory)][string]$RelPyPath)
            $normRel = $RelPyPath.Replace('\', '/')
            $fullPy = Join-Path $script:RepoRoot $normRel
            $fullPs1 = [System.IO.Path]::ChangeExtension($fullPy, '.ps1')
            $onDisk = Test-Path -LiteralPath $fullPy -PathType Leaf
            $diskSha = $null
            if ($onDisk) {
                $diskBytes = [System.IO.File]::ReadAllBytes($fullPy)
                $diskSha = [System.Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($diskBytes)).ToLowerInvariant()
            }
            $catRes = Invoke-GeneratorCommand -Executable 'git' -Arguments @('--no-optional-locks', 'cat-file', '-e', "HEAD:$normRel")
            $inHead = ($catRes.ExitCode -eq 0)
            $headSha = $null
            if ($inHead) {
                $showRes = Invoke-GeneratorCommand -Executable 'git' -Arguments @('--no-optional-locks', 'show', "HEAD:$normRel")
                if ($showRes.ExitCode -eq 0) {
                    $headBytes = [System.Text.Encoding]::UTF8.GetBytes($showRes.StdOut)
                    $headSha = [System.Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($headBytes)).ToLowerInvariant()
                }
            }
            return [pscustomobject]@{
                RelPyPath      = $normRel
                Ps1Exists      = (Test-Path -LiteralPath $fullPs1 -PathType Leaf)
                OnDisk         = $onDisk
                DiskSha256     = $diskSha
                InGitHead      = $inHead
                HeadSha256     = $headSha
                EditedFromHead = ($onDisk -and ($diskSha -ne $headSha))
            }
        }

        function Assert-DynamicPythonGateParity {
            param(
                [Parameter(Mandatory)][string]$RelPyPath,
                [string[]]$PyArguments = @('--check'),
                [string]$PyOutputFile = '',
                [string]$ExpectedSha256 = ''
            )
            $state = Get-PythonGateSourceState -RelPyPath $RelPyPath
            $state.Ps1Exists | Should -BeTrue
            if ($state.OnDisk -and $script:PyRuntime.Available) {
                $pyScript = Join-Path $script:RepoRoot $state.RelPyPath
                $allArgs = @($script:PyRuntime.PrefixArgs) + @($pyScript) + $PyArguments
                $pyRes = Invoke-GeneratorCommand -Executable $script:PyRuntime.Exe -Arguments $allArgs
                $pyRes.ExitCode | Should -Be 0
                if ($PyOutputFile -ne '' -and $ExpectedSha256 -ne '') {
                    $pyMetrics = Get-ArtifactMetrics -FilePath $PyOutputFile
                    $pyMetrics.Sha256 | Should -Be $ExpectedSha256
                }
            }
        }

        $script:PyRuntime = Resolve-PythonCommand
    }

    It 'inspects Get-PythonGateSourceState across all converted schemagen and npcabilities gates' {
        $gatePyPaths = @(
            'src/tools/schemagen/gen_zoi_surface.py',
            'src/tools/schemagen/gen_region_rects.py',
            'src/tools/schemagen/gen_region.py',
            'src/tools/schemagen/gen_gm_garden.py',
            'src/tools/schemagen/gen_talent_fixture.py',
            'src/tools/schemagen/gen_gear_fixture.py',
            'src/tools/schemagen/gen_tooltips.py',
            'src/tools/schemagen/gen_town_layouts.py',
            'src/tools/npcabilities/targeting.py',
            'src/tools/npcabilities/looks.py'
        )
        foreach ($relPy in $gatePyPaths) {
            $state = Get-PythonGateSourceState -RelPyPath $relPy
            $state.Ps1Exists | Should -BeTrue
            ($state.OnDisk -or $state.InGitHead) | Should -BeTrue
        }
    }

    It 'verifies scripts/generators.ps1 -Only node_constants and scripts/wsl/gen-node-constants.sh parity (2 artifacts)' {
        $res = Invoke-PwshGenerator -RelScriptPath 'scripts/generators.ps1' -Arguments @('-Only', 'node_constants')
        $res.ExitCode | Should -Be 0

        $goArtifact = Join-Path $script:RepoRoot 'build\gen\go\citynodeconsts\constants_gen.go'
        $cppArtifact = Join-Path $script:RepoRoot 'build\gen\world\world\node_constants.h'
        $goSha = '3fa397cc4befcaae66decafa4d0fb6d01bb5e5297ed98ca9e536a1dfd7b4b712'
        $cppSha = 'f1be7d3b288911d4fa1b9153a8dfb76cbd83653f626b917dd67a6d9e51081bf3'

        Assert-GoldenArtifact -FilePath $goArtifact -ExpectedBytes 3800 -AllowedSha256 @($goSha) -ExpectedLfLines 117
        Assert-GoldenArtifact -FilePath $cppArtifact -ExpectedBytes 5001 -AllowedSha256 @($cppSha) -ExpectedLfLines 127

        $shExe = Resolve-PosixShExecutable
        if ($shExe -ne '') {
            $checkRes = Invoke-GeneratorCommand -Executable $shExe -Arguments @('./scripts/wsl/gen-node-constants.sh', '--check')
            $checkRes.ExitCode | Should -Be 0
            if ($shExe -like '*\Git\bin\sh.exe') {
                $tmpGo = (Join-Path $TestDrive 'constants_gen.go').Replace('\', '/')
                $tmpCpp = (Join-Path $TestDrive 'node_constants.h').Replace('\', '/')
                $shOutRes = Invoke-GeneratorCommand -Executable $shExe -Arguments @('./scripts/wsl/gen-node-constants.sh', '--go-out', $tmpGo, '--cpp-out', $tmpCpp)
                $shOutRes.ExitCode | Should -Be 0
                Assert-GoldenArtifact -FilePath (Join-Path $TestDrive 'constants_gen.go') -ExpectedBytes 3800 -AllowedSha256 @($goSha) -ExpectedLfLines 117
                Assert-GoldenArtifact -FilePath (Join-Path $TestDrive 'node_constants.h') -ExpectedBytes 5001 -AllowedSha256 @($cppSha) -ExpectedLfLines 127
            }
        }
    }

    It 'verifies gen_zoi_surface.ps1 --check and zoi_surface_table.h parity (1 artifact)' {
        $artifact = Join-Path $script:RepoRoot 'build\gen\world\world\zoi_surface_table.h'
        $expectedSha = '86404d2b4f8eddea2dc0ec3ae6e6d9da75864285e851bf76d01ce898b9b9bee0'
        Assert-GoldenArtifact -FilePath $artifact -ExpectedBytes 1098074 -AllowedSha256 @($expectedSha) -ExpectedLfLines 9539
        if ($script:HasContentAssets) {
            Assert-DynamicPythonGateParity -RelPyPath 'src/tools/schemagen/gen_zoi_surface.py'
            $res = Invoke-PwshGenerator -RelScriptPath 'src/tools/schemagen/gen_zoi_surface.ps1' -Arguments @('--check')
            $res.ExitCode | Should -Be 0
        }
    }

    It 'verifies gen_region_rects.ps1 --check and riverlands_rects.json / riverlands_zoi_rects.json parity (2 artifacts)' {
        $rectsJson = Join-Path $script:RepoRoot 'build\gen\fixtures\region\riverlands_rects.json'
        $zoiJson = Join-Path $script:RepoRoot 'build\gen\fixtures\region\riverlands_zoi_rects.json'
        $allowedRectsSha = @(
            '9099008355c3e6b54922d3461ae9cb28b9a3b9280b7afed82e389e87f848ea29',
            '4d2563a267e4996c6f2c96ecde908cf2831385933ab063e2e34ad18463b451c5'
        )
        $allowedZoiSha = @(
            '6e7891d566d34418cad80ebd23ac407bc27ced34ccbe302e76b00148f2152f19',
            '82ed75453a0b4a05019d40d70a8250e7cff3eae58c7a7b5694acf9dc49c0a229'
        )
        Assert-GoldenArtifact -FilePath $rectsJson -ExpectedBytes 13312 -AllowedSha256 $allowedRectsSha
        Assert-GoldenArtifact -FilePath $zoiJson -ExpectedBytes 56521 -AllowedSha256 $allowedZoiSha
        if ($script:HasContentAssets) {
            Assert-DynamicPythonGateParity -RelPyPath 'src/tools/schemagen/gen_region_rects.py'
            $res = Invoke-PwshGenerator -RelScriptPath 'src/tools/schemagen/gen_region_rects.ps1' -Arguments @('--check')
            $res.ExitCode | Should -Be 0
        }
    }

    It 'verifies gen_region.ps1 --check and region_data.h / region_rects.tsv parity (2 artifacts)' {
        $regionDataH = Join-Path $script:RepoRoot 'build\gen\world\spawns\region_data.h'
        $regionRectsTsv = Join-Path $script:RepoRoot 'build\gen\nav\region_rects.tsv'
        $expectedHeaderSha = '087ebebac6d1a7d81842e94a9be627d0467561fe1292c733b82000aac41c0610'
        $allowedNavTsvSha = @(
            '512be87394139759afb70500038020dd20d5645fc8a305db622995e2af1290e9',
            'cbbeead5ebe570607fc7fcd736f0d131b15e753604008a8d0ef9dcaf19a78aec'
        )
        Assert-GoldenArtifact -FilePath $regionDataH -ExpectedBytes 88143 -AllowedSha256 @($expectedHeaderSha) -ExpectedLfLines 2148
        Assert-GoldenArtifact -FilePath $regionRectsTsv -ExpectedBytes 14497 -AllowedSha256 $allowedNavTsvSha -ExpectedLfLines 403
        Assert-DynamicPythonGateParity -RelPyPath 'src/tools/schemagen/gen_region.py'
        $res = Invoke-PwshGenerator -RelScriptPath 'src/tools/schemagen/gen_region.ps1' -Arguments @('--check')
        $res.ExitCode | Should -Be 0
    }

    It 'verifies gen_gm_garden.ps1 --check and garden_defs.h parity (1 artifact)' {
        $gardenDefsH = Join-Path $script:RepoRoot 'build\gen\world\spawns\garden_defs.h'
        $expectedSha = 'f0dd0ef6a5b4bd5dcb6fbba8658056980951a5bd3d2ba466c7fa60633d3a9aa8'
        Assert-GoldenArtifact -FilePath $gardenDefsH -ExpectedBytes 3892 -AllowedSha256 @($expectedSha) -ExpectedLfLines 61
        if ($script:HasContentAssets) {
            Assert-DynamicPythonGateParity -RelPyPath 'src/tools/schemagen/gen_gm_garden.py'
            $res = Invoke-PwshGenerator -RelScriptPath 'src/tools/schemagen/gen_gm_garden.ps1' -Arguments @('--check')
            $res.ExitCode | Should -Be 0
        }
    }

    It 'verifies gen_talent_fixture.ps1 temp output and talent_fixture.sql parity (1 artifact)' {
        $expectedSha = '8579630d2006f9e4bd7b8f80d5548124d50d7cc2c5fc92bd93f0a52b7342c027'
        $goldenFile = Join-Path $script:RepoRoot 'build\gen\world\tests\talent_fixture.sql'
        Assert-GoldenArtifact -FilePath $goldenFile -ExpectedBytes 969463 -AllowedSha256 @($expectedSha) -ExpectedLfLines 4274
        if ($script:HasContentAssets) {
            $tmpOut = Join-Path $TestDrive 'talent_fixture.sql'
            $res = Invoke-PwshGenerator -RelScriptPath 'src/tools/schemagen/gen_talent_fixture.ps1' -Arguments @($script:ContentDb, $tmpOut)
            $res.ExitCode | Should -Be 0
            Assert-GoldenArtifact -FilePath $tmpOut -ExpectedBytes 969463 -AllowedSha256 @($expectedSha) -ExpectedLfLines 4274
            $pyTmpOut = Join-Path $TestDrive 'talent_fixture_py.sql'
            Assert-DynamicPythonGateParity -RelPyPath 'src/tools/schemagen/gen_talent_fixture.py' -PyArguments @($script:ContentDb, $pyTmpOut) -PyOutputFile $pyTmpOut -ExpectedSha256 $expectedSha
        }
    }

    It 'verifies gen_gear_fixture.ps1 temp output and gear_fixture.h parity (1 artifact)' {
        $expectedSha = 'e26402e2a51b8dca8816df89b53fb4d054a2729e14f897daa86814351d969d2f'
        $goldenFile = Join-Path $script:RepoRoot 'build\gen\world\tests\gear_fixture.h'
        Assert-GoldenArtifact -FilePath $goldenFile -ExpectedBytes 53342 -AllowedSha256 @($expectedSha) -ExpectedLfLines 415
        if ($script:HasContentAssets) {
            $tmpOut = Join-Path $TestDrive 'gear_fixture.h'
            $res = Invoke-PwshGenerator -RelScriptPath 'src/tools/schemagen/gen_gear_fixture.ps1' -Arguments @($script:ContentDb, $tmpOut)
            $res.ExitCode | Should -Be 0
            Assert-GoldenArtifact -FilePath $tmpOut -ExpectedBytes 53342 -AllowedSha256 @($expectedSha) -ExpectedLfLines 415
            $pyTmpOut = Join-Path $TestDrive 'gear_fixture_py.h'
            Assert-DynamicPythonGateParity -RelPyPath 'src/tools/schemagen/gen_gear_fixture.py' -PyArguments @($script:ContentDb, $pyTmpOut) -PyOutputFile $pyTmpOut -ExpectedSha256 $expectedSha
        }
    }

    It 'verifies gen_tooltips.ps1 --check and tooltip_key.h parity (1 artifact)' {
        $expectedSha = 'fa97cedc5ed787408bd4911e48ea17ec2058e7c240df33f333156eb0b8d94441'
        $goldenFile = Join-Path $script:RepoRoot 'build\gen\world\tests\tooltip_key.h'
        Assert-GoldenArtifact -FilePath $goldenFile -ExpectedBytes 123696 -AllowedSha256 @($expectedSha) -ExpectedLfLines 318
        if ($script:HasContentAssets) {
            Assert-DynamicPythonGateParity -RelPyPath 'src/tools/schemagen/gen_tooltips.py'
            $res = Invoke-PwshGenerator -RelScriptPath 'src/tools/schemagen/gen_tooltips.ps1' -Arguments @('--check')
            $res.ExitCode | Should -Be 0
        }
    }

    It 'verifies gen_town_layouts.ps1 temp output and town_layout_items.tsv parity (1 artifact)' {
        $expectedSha = 'ee06d0fca7981ca07d70e2e5d4c260c54725cd269f49c71d1f8f481bf3d23a2d'
        $goldenFile = Join-Path $script:RepoRoot 'dist\content\town_layout_items.tsv'
        if (Test-Path -LiteralPath $goldenFile -PathType Leaf) {
            Assert-GoldenArtifact -FilePath $goldenFile -ExpectedBytes 5707813 -AllowedSha256 @($expectedSha) -ExpectedLfLines 38975
        }
        if ($script:HasContentAssets) {
            $tmpOut = Join-Path $TestDrive 'town_layout_items.tsv'
            $res = Invoke-PwshGenerator -RelScriptPath 'src/tools/schemagen/gen_town_layouts.ps1' -Arguments @('--content-db', $script:ContentDb, '--out', $tmpOut)
            $res.ExitCode | Should -Be 0
            Assert-GoldenArtifact -FilePath $tmpOut -ExpectedBytes 5707813 -AllowedSha256 @($expectedSha) -ExpectedLfLines 38975
            $pyTmpOut = Join-Path $TestDrive 'town_layout_items_py.tsv'
            Assert-DynamicPythonGateParity -RelPyPath 'src/tools/schemagen/gen_town_layouts.py' -PyArguments @('--content-db', $script:ContentDb, '--out', $pyTmpOut) -PyOutputFile $pyTmpOut -ExpectedSha256 $expectedSha
        }
    }

    It 'verifies npcabilities/targeting.ps1 temp output and npc_targeting.tsv parity (1 artifact)' {
        $expectedSha = '1e93532d53ebfff3b1be0136e5909ab5779b1274d4cdcc115eea670dbe78c702'
        $goldenFile = Join-Path $script:RepoRoot 'dist\content\npc_targeting.tsv'
        $abilitiesFile = Join-Path $script:RepoRoot 'dist\content\npc_abilities.tsv'
        if (Test-Path -LiteralPath $goldenFile -PathType Leaf) {
            Assert-GoldenArtifact -FilePath $goldenFile -ExpectedBytes 68595 -AllowedSha256 @($expectedSha) -ExpectedLfLines 959
        }
        if ($script:HasContentAssets) {
            $tmpOut = Join-Path $TestDrive 'npc_targeting.tsv'
            $res = Invoke-PwshGenerator -RelScriptPath 'src/tools/npcabilities/targeting.ps1' -Arguments @('--content-db', $script:ContentDb, '--abilities', $abilitiesFile, '--out', $tmpOut)
            $res.ExitCode | Should -Be 0
            Assert-GoldenArtifact -FilePath $tmpOut -ExpectedBytes 68595 -AllowedSha256 @($expectedSha) -ExpectedLfLines 959
            $pyTmpOut = Join-Path $TestDrive 'npc_targeting_py.tsv'
            Assert-DynamicPythonGateParity -RelPyPath 'src/tools/npcabilities/targeting.py' -PyArguments @('--content-db', $script:ContentDb, '--abilities', $abilitiesFile, '--out', $pyTmpOut) -PyOutputFile $pyTmpOut -ExpectedSha256 $expectedSha
        }
    }

    It 'verifies npcabilities/looks.ps1 temp output and npc_appearances.tsv / npc_names.tsv parity (2 artifacts)' {
        $appSha = '38d38c92da1441a99a4432a143a76fa33fc76a721d2709e17b307582457c8256'
        $namesSha = 'd1c9ddc7c0d71453671a8a693ac0f83dd5200dadd779820b4ea3034a4709f23c'
        $goldenApp = Join-Path $script:RepoRoot 'dist\content\npc_appearances.tsv'
        $goldenNames = Join-Path $script:RepoRoot 'dist\content\npc_names.tsv'
        if ((Test-Path -LiteralPath $goldenApp -PathType Leaf) -and (Test-Path -LiteralPath $goldenNames -PathType Leaf)) {
            Assert-GoldenArtifact -FilePath $goldenApp -ExpectedBytes 102570 -AllowedSha256 @($appSha) -ExpectedLfLines 1793
            Assert-GoldenArtifact -FilePath $goldenNames -ExpectedBytes 172521 -AllowedSha256 @($namesSha) -ExpectedLfLines 3507
        }
        if ($script:HasContentAssets) {
            $tmpApp = Join-Path $TestDrive 'npc_appearances.tsv'
            $tmpNames = Join-Path $TestDrive 'npc_names.tsv'
            $res = Invoke-PwshGenerator -RelScriptPath 'src/tools/npcabilities/looks.ps1' -Arguments @('--content-db', $script:ContentDb, '--appearances-out', $tmpApp, '--names-out', $tmpNames)
            $res.ExitCode | Should -Be 0
            Assert-GoldenArtifact -FilePath $tmpApp -ExpectedBytes 102570 -AllowedSha256 @($appSha) -ExpectedLfLines 1793
            Assert-GoldenArtifact -FilePath $tmpNames -ExpectedBytes 172521 -AllowedSha256 @($namesSha) -ExpectedLfLines 3507
            $pyTmpApp = Join-Path $TestDrive 'npc_appearances_py.tsv'
            $pyTmpNames = Join-Path $TestDrive 'npc_names_py.tsv'
            Assert-DynamicPythonGateParity -RelPyPath 'src/tools/npcabilities/looks.py' -PyArguments @('--content-db', $script:ContentDb, '--appearances-out', $pyTmpApp, '--names-out', $pyTmpNames) -PyOutputFile $pyTmpApp -ExpectedSha256 $appSha
        }
    }

    It 'verifies scripts/re_workbench.ps1 -Action test exits 0 across all RE workbench suites' {
        $res = Invoke-PwshGenerator -RelScriptPath 'scripts/re_workbench.ps1' -Arguments @('-Action', 'test')
        $res.ExitCode | Should -Be 0
        $res.StdOut | Should -Match '\[PASS\]'
    }
}
