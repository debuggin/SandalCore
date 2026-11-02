Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$inPester = ($null -ne (Get-Module -Name Pester)) -and (
    @(Get-PSCallStack | Where-Object {
        ($null -ne $_.Command -and $_.Command -match 'Pester') -or
        ($null -ne $_.ScriptName -and $_.ScriptName -match 'Pester')
    }).Count -gt 0
)

if (-not $inPester) {
    function Describe([string]$Name, [scriptblock]$Fixture) {
        Write-Host "[Describe] $Name"
        & $Fixture
    }
    function Context([string]$Name, [scriptblock]$Fixture) {
        Write-Host "  [Context] $Name"
        & $Fixture
    }
    function BeforeAll([scriptblock]$Action) {
        & $Action
    }
    function It([string]$Name, [scriptblock]$Test) {
        & $Test
        Write-Host "    [PASS] $Name"
    }
}

Describe 'SandalCore Public Hygiene Check, Heavy Lock & Nav Stamp Decoupling' {
    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
        $script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
        $script:HeavyLockScript = Join-Path $script:RepoRoot 'scripts\checks\heavy_lock.ps1'
        $script:ExtractGameDataScript = Join-Path $script:RepoRoot 'scripts\extract_game_data.ps1'
    }

    Context 'public_check.ps1 Repository Verification' {
        It 'exits 0 and emits [PASS] public_check.ps1 verified on the repository' {
            $output = & pwsh -NoProfile -File $script:PublicCheckScript
            $exitCode = $LASTEXITCODE
            if ($exitCode -ne 0) {
                throw "Expected public_check.ps1 to exit 0, got $exitCode. Output: $($output -join ' ')"
            }
            $joined = ($output -join "`n")
            if ($joined -notmatch '\[PASS\] public_check\.ps1 verified') {
                throw "Expected '[PASS] public_check.ps1 verified' in output, got: $joined"
            }
        }

        It 'rejects forbidden .inc files, PEM headers, and PE32+ image-base literals in an isolated fixture' {
            $tmpRepo = Join-Path ([System.IO.Path]::GetTempPath()) ("sandal-pubcheck-" + [guid]::NewGuid().ToString('N'))
            try {
                $null = New-Item -ItemType Directory -Path (Join-Path $tmpRepo 'tests') -Force
                $null = New-Item -ItemType Directory -Path (Join-Path $tmpRepo 'scripts\checks') -Force
                & git -C $tmpRepo init -q
                if ($LASTEXITCODE -ne 0) {
                    throw "git init failed with exit code $LASTEXITCODE"
                }
                $leakFile = Join-Path $tmpRepo 'tests\sample.inc'
                $pemHeader = 'BEGIN ' + 'PRIVATE KEY'
                $peLiteral = '0x140' + '001234'
                Set-Content -LiteralPath $leakFile -Value "$pemHeader`n$peLiteral" -Encoding utf8
                $out = & pwsh -NoProfile -File $script:PublicCheckScript -RepoRoot $tmpRepo
                $code = $LASTEXITCODE
                if ($code -ne 1) {
                    throw "Expected public_check.ps1 to exit 1 on contaminated fixture, got $code. Output: $($out -join ' ')"
                }
            } finally {
                if (Test-Path -LiteralPath $tmpRepo) {
                    Remove-Item -LiteralPath $tmpRepo -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'heavy_lock.ps1 Named Mutex & Lockfile Lifecycle' {
        It 'executes a scriptblock under Global\SandalCore_HeavyOperation_Lock and writes/cleans data/.stamps/heavy_lock.json' {
            $dataDir = Join-Path $script:RepoRoot 'data'
            $lockFile = Join-Path $script:RepoRoot 'data\.stamps\heavy_lock.json'
            $stampDir = Split-Path -Parent $lockFile
            try {
                if (-not (Test-Path -LiteralPath $stampDir)) {
                    $null = New-Item -ItemType Directory -Path $stampDir -Force
                }
                $stalePayload = [ordered]@{
                    pid            = 999999
                    operation      = 'stale-op'
                    started_at_utc = [datetime]::UtcNow.AddMinutes(-10).ToString('o')
                    command_line   = 'stale'
                } | ConvertTo-Json
                Set-Content -LiteralPath $lockFile -Value $stalePayload -Encoding utf8

                $state = @{ ObservedLock = $null }
                & $script:HeavyLockScript -Operation 'unit-test-op' -Action {
                    if (-not (Test-Path -LiteralPath $lockFile)) {
                        throw 'Expected data/.stamps/heavy_lock.json to exist while Action is running.'
                    }
                    $state.ObservedLock = Get-Content -LiteralPath $lockFile -Raw | ConvertFrom-Json
                }

                if ($null -eq $state.ObservedLock) {
                    throw 'Expected Action scriptblock to capture heavy_lock.json content.'
                }
                if ([int]$state.ObservedLock.pid -ne $PID) {
                    throw "Expected lockfile pid $PID, got $($state.ObservedLock.pid)"
                }
                if ($state.ObservedLock.operation -ne 'unit-test-op') {
                    throw "Expected lockfile operation 'unit-test-op', got '$($state.ObservedLock.operation)'"
                }
                if (Test-Path -LiteralPath $lockFile) {
                    throw 'Expected data/.stamps/heavy_lock.json to be cleaned up after heavy_lock.ps1 completes.'
                }
            } finally {
                if ((Test-Path -LiteralPath $stampDir) -and @(Get-ChildItem -LiteralPath $stampDir -Force).Count -eq 0) {
                    Remove-Item -LiteralPath $stampDir -Force -ErrorAction SilentlyContinue
                }
                if ((Test-Path -LiteralPath $dataDir) -and @(Get-ChildItem -LiteralPath $dataDir -Force).Count -eq 0) {
                    Remove-Item -LiteralPath $dataDir -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'extract_game_data.ps1 Get-CodeNav CMakeLists Decoupling (SC-02)' {
        It 'ensures Get-CodeNav does not include CMakeLists.txt or server\worldd\CMakeLists.txt' {
            $content = Get-Content -LiteralPath $script:ExtractGameDataScript -Raw
            $match = [regex]::Match($content, '(?m)^\s*function\s+Get-CodeNav\s*\{[^\r\n]+\}')
            if (-not $match.Success) {
                throw 'Could not locate function Get-CodeNav in scripts/extract_game_data.ps1'
            }
            $line = $match.Value
            if ($line -match 'CMakeLists\.txt') {
                throw "Expected Get-CodeNav not to reference CMakeLists.txt, but found: $line"
            }
            if ($line -notmatch 'server\\worldd\\nav' -or $line -notmatch 'third_party\\recastnavigation' -or $line -notmatch 'vcpkg\.json') {
                throw "Expected Get-CodeNav to retain nav, recastnavigation, and vcpkg.json inputs, got: $line"
            }
        }
    }
}
