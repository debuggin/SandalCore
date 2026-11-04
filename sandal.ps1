#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Command = 'help',
    [switch]$Json,
    [switch]$NoColor,
    [switch]$DryRun,
    [switch]$Help,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ValidCommands = @('doctor', 'status', 'up', 'down', 'extract', 'certs', 'build', 'deploy', 'test', 'run', 'install-wsl-docker', 'help')
$script:UseNoColor = [bool]($NoColor -or -not [string]::IsNullOrEmpty($env:NO_COLOR))

function Write-SandalUsage {
    param([switch]$AsJson)
    if ($AsJson) {
        Write-Output (@{
            ok       = $true
            command  = 'help'
            usage    = 'pwsh ./sandal.ps1 <doctor|status|up|down|extract|certs|build|deploy|test|run|install-wsl-docker|help> [-Json] [-NoColor] [-DryRun]'
            commands = $script:ValidCommands
        } | ConvertTo-Json -Depth 4)
        return
    }
    Write-Output 'Usage: pwsh ./sandal.ps1 <command> [-Json] [-NoColor] [-DryRun]'
    Write-Output 'Commands: doctor, status, up, down, extract, certs, build, deploy, test, run, install-wsl-docker, help'
}

function Invoke-SandalStatus {
    param([string]$RepoRoot, [switch]$AsJson)
    $stages = [System.Collections.Generic.List[object]]::new()
    foreach ($stage in (Get-SandalStageOrder)) {
        $stamp = Read-SandalStageStamp -Stage $stage -RepoRoot $RepoRoot
        $stages.Add([pscustomobject]@{
            stage      = $stage
            stamped    = ($null -ne $stamp)
            input_hash = if ($null -ne $stamp) { $stamp.input_hash } else { $null }
        })
    }
    if ($AsJson) {
        Write-Output (@{
            ok       = $true
            command  = 'status'
            no_color = $script:UseNoColor
            stages   = $stages.ToArray()
        } | ConvertTo-Json -Depth 5)
        return
    }
    foreach ($item in $stages) {
        $state = if ($item.stamped) { 'STAMPED' } else { 'PENDING' }
        Write-Output ("[{0}] {1}" -f $state, $item.stage)
    }
}

function Invoke-SandalSubcommand {
    param([string]$Subcommand, [string]$RepoRoot, [switch]$AsJson, [switch]$IsDryRun, [string[]]$SubcommandArgs = @())
    if ($Subcommand -eq 'status') {
        Invoke-SandalStatus -RepoRoot $RepoRoot -AsJson:$AsJson
        return
    }
    $scriptFile = if ($Subcommand -eq 'install-wsl-docker') { 'Install-WslDocker.ps1' } else { "${Subcommand}.ps1" }
    $targetScript = Join-Path $RepoRoot "scripts/sandal/${scriptFile}"
    if (-not (Test-Path -LiteralPath $targetScript -PathType Leaf)) {
        throw "Subcommand script not found: $targetScript"
    }
    $global:LASTEXITCODE = 0
    if ($Subcommand -eq 'install-wsl-docker') {
        & $targetScript -DryRun:$IsDryRun -Json:$AsJson -NoColor:$script:UseNoColor @SubcommandArgs
    } else {
        & $targetScript -DryRun:$IsDryRun -Json:$AsJson -NoColor:$script:UseNoColor -Arguments $SubcommandArgs
    }
    $code = if ($null -ne $LASTEXITCODE) { [int]$LASTEXITCODE } else { 0 }
    exit $code
}

$normCmd = if ($Help -or $Command -in @('--help', '-h', '/?')) { 'help' } else { $Command.ToLowerInvariant() }
if ($normCmd -eq 'help') {
    Write-SandalUsage -AsJson:$Json
    exit 0
}
if ($normCmd -notin $script:ValidCommands) {
    [Console]::Error.WriteLine("What: Unknown SandalCore subcommand '$Command'. Why: Must be one of $($script:ValidCommands -join ', '). How-to-fix: Run 'pwsh ./sandal.ps1 help' for valid usage (Exit 2).")
    exit 2
}

try {
    Import-Module (Join-Path $PSScriptRoot 'scripts\lib\SandalCore.psd1') -Force -DisableNameChecking
    Invoke-SandalSubcommand -Subcommand $normCmd -RepoRoot $PSScriptRoot -AsJson:$Json -IsDryRun:$DryRun -SubcommandArgs $RemainingArgs
    exit 0
} catch {
    [Console]::Error.WriteLine("What: SandalCore '$normCmd' failed. Why: $($_.Exception.Message) How-to-fix: Run 'pwsh ./sandal.ps1 doctor' to inspect prerequisites (Exit 1).")
    exit 1
}
