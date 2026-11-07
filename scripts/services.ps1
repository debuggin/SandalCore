<#
  Go services helper.
    scripts\services.ps1 up      start PostgreSQL (docker compose, waits for healthy)
    scripts\services.ps1 test    buf lint + generated-code check, go vet + go test (needs 'up'); the tests of the
                                 character creation rules and of the character service's content.db use
                                 $env:AOC_CONTENT_DB (or dist\content\content.db) when that file exists, and skip without it
    scripts\services.ps1 build   build auth, character, citynode, nodectl, quests, questctl, worldctl, charimport, mesh, gateway into dist\services
    scripts\services.ps1 run     build, then start auth, character, citynode, quests, mesh, gateway and world hidden; each logs
                                 to dist\services\logs\<name>.log (the previous run's: <name>.prev.log).  citynode
                                 (Nodes 1) serves the city nodes on :50055 and applies src\config\seeds\citynode_seed.json once, at its
                                 first start; dist\services\nodectl.exe is its command line (src\server\services\README.md, "City nodes").
                                 worldctl.exe (the GM travel tools: players, places, tp, speed) talks to world's admin API on
                                 :50056 (src\server\services\README.md, "GM travel tools")
                                 world (dist\Release\world.exe, scripts\build.ps1 -Target world) registers with mesh;
                                 there is no static world.  world gets
                                 --content-db dist\content\content.db (or $env:AOC_CONTENT_DB): the file
                                 contentc builds; without it world starts but refuses every login.
                                 $env:AOC_GAMESTATE_MODE (tier1, tier2, tier3, captured; default tier3) reaches world as it is.
                                 The character service gets the same --content-db (plan B1): it checks a new
                                 character's race, gender, class and customization against it; without it
                                 the service starts but refuses to create characters, saying why
    scripts\services.ps1 stop    stop those processes, world included (drops the gateway's live
                                 client sessions); needs neither Go nor Docker
    scripts\services.ps1 login   sign in with src\config\dev_login.local.json and save the session token
    scripts\services.ps1 down    stop the containers (data volume kept)
  contentc, the content.db builder (plan C1), is not built or started here: see src\server\services\README.md.
  run creates dist\services\service.token once: the internal services' shared secret, readable
  by this user only.
#>
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('up', 'test', 'build', 'run', 'stop', 'login', 'down')] [string] $Action
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $root 'scripts' 'lib' 'SandalCore.psd1') -Force -DisableNameChecking
$compose  = Join-Path $root 'deploy' 'compose.yml'
$envFile  = Join-Path $root 'deploy' '.env'
$svc      = Join-Path $root 'src' 'server' 'services'
$out      = Join-Path $root 'dist' 'services'
$logs     = Join-Path $out 'logs'
$exeExt   = if ($IsWindows) { '.exe' } else { '' }
$pathSep  = [System.IO.Path]::PathSeparator
$userHome = if ($env:USERPROFILE) { $env:USERPROFILE } else { $HOME }

function Ensure-GoProtoGen {
    $genDir = Join-Path $root 'build' 'gen' 'go'
    New-Item -ItemType Directory -Force -Path $genDir | Out-Null
    $savedPath = $env:Path
    $miseBase = if ($env:LOCALAPPDATA) { $env:LOCALAPPDATA } else { Join-Path $userHome '.local' 'share' }
    $miseShims = Join-Path $miseBase 'mise' 'shims'
    $goBin = Join-Path (& go env GOPATH) 'bin'
    $goUserBin = Join-Path $userHome 'go' 'bin'
    $env:Path = @($miseShims, $goBin, $goUserBin, $env:Path) -join $pathSep
    Push-Location $svc
    try {
        & buf generate ../../libs/aocproto/proto/internal
        if ($LASTEXITCODE -ne 0) { throw 'buf generate ../../libs/aocproto/proto/internal failed' }
        if (Test-Path '../../libs/aocproto/proto/ics_xclient.proto') {
            & buf generate ../../libs/aocproto/proto/ics_xclient.proto
            if ($LASTEXITCODE -ne 0) { throw 'buf generate ../../libs/aocproto/proto/ics_xclient.proto failed' }
        }
    } finally {
        Pop-Location
        $env:Path = $savedPath
    }
    $goModPath = Join-Path $genDir 'go.mod'
    $goModContent = "module aocrop/services/gen`n`ngo 1.24.0`n`nrequire (`n`tgoogle.golang.org/grpc v1.79.3`n`tgoogle.golang.org/protobuf v1.36.11`n)`n"
    [System.IO.File]::WriteAllText($goModPath, $goModContent, [System.Text.UTF8Encoding]::new($false))
    $nodeConsts = Join-Path $genDir 'citynodeconsts' 'constants_gen.go'
    if (-not (Test-Path -LiteralPath $nodeConsts)) {
        $ini = Join-Path $root 'dist' 'content' 'client_config' 'DefaultAoCRecordConstants.ini'
        if (Test-Path -LiteralPath $ini) {
            & pwsh -NoProfile -File (Join-Path $root 'scripts' 'generators.ps1') -Only node_constants
            if ($LASTEXITCODE -ne 0) { throw "scripts/generators.ps1 -Only node_constants failed (exit $LASTEXITCODE)" }
        }
    }
}

function Build {
    Ensure-GoProtoGen
    New-Item -ItemType Directory -Force $out | Out-Null
    Push-Location $svc
    try {
        foreach ($name in 'auth', 'character', 'citynode', 'nodectl', 'quests', 'questctl', 'worldctl', 'charimport', 'mesh', 'gateway', 'mesh-gateway', 'admin-gateway') {
            $pkg = if ($name -eq 'quests') { './cmd/quest' } else { "./cmd/$name" }
            & go build -o (Join-Path $out "$name$exeExt") $pkg
            if ($LASTEXITCODE -ne 0) { throw "go build $name failed" }
        }
    } finally { Pop-Location }
    Write-Host "built into $out"
}
# [C O A + Cyb]

$worldExe = Join-Path $root 'dist' 'Release' "world$exeExt"
$tokenFile = Join-Path $out 'service.token'
# content.db, the file world values characters from and the character service checks new characters against
# (plan C1's contentc, src\server\services\README.md "Content").
$contentDb = if ($env:AOC_CONTENT_DB) { $env:AOC_CONTENT_DB } else { Join-Path $root 'dist' 'content' 'content.db' }

# The auth, character, citynode, quests, mesh, gateway and world processes running from this
# checkout (not another checkout's copy).
function Get-ServiceProcess {
    $exes = @('auth', 'character', 'citynode', 'quests', 'mesh', 'gateway' | ForEach-Object { Join-Path $out "$_$exeExt" }) + @($worldExe)
    Get-Process -Name auth, character, citynode, quests, mesh, gateway, world -ErrorAction SilentlyContinue |
        Where-Object { $exes -contains $_.Path }
}

function Stop-ServiceProcesses($procs) { # WHY: stop world first so its gRPC streams close from its side
    $worlds = @($procs | Where-Object { $_.Name -eq 'world' })
    $others = @($procs | Where-Object { $_.Name -ne 'world' })
    $worlds | Stop-Process -Force -ErrorAction SilentlyContinue
    $worlds | ForEach-Object { $null = $_.WaitForExit(5000) }
    $others | Stop-Process -Force -ErrorAction SilentlyContinue
    $others | ForEach-Object { $null = $_.WaitForExit(5000) }
}

# Lets only this user open $path (like the launcher's saved login token).
function Set-OwnerOnly($path) {
    if ($IsWindows) {
        & icacls $path /inheritance:r /grant:r "${env:USERDOMAIN}\${env:USERNAME}:(F)" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "could not restrict $path to ${env:USERNAME}" }
    } else {
        & chmod 600 $path
        if ($LASTEXITCODE -ne 0) { throw "could not chmod 600 $path" }
    }
}

# The internal services' shared secret (src/server/services/internal/svcauth): 32 random
# bytes as 64 lowercase hex digits, created once.
function New-ServiceToken {
    if (Test-Path $tokenFile) { return }
    New-Item -ItemType Directory -Force $out | Out-Null
    $bytes = New-Object byte[] 32
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    [System.IO.File]::WriteAllText($tokenFile, (-join ($bytes | ForEach-Object { $_.ToString('x2') })))
    Set-OwnerOnly $tokenFile
    Write-Host "created the service token $tokenFile"
}

# "auth (pid 12), character (pid 34)"; call it before the processes exit.
function Format-Process($procs) {
    ($procs | ForEach-Object { '{0} (pid {1})' -f $_.Name, $_.Id }) -join ', '
}

try {
    $env:COMPOSE_CONVERT_WINDOWS_PATHS = '1'
    switch ($Action) {
        'up' {
            $null = Sync-SandalDeployStage -RepoRoot $root
            $null = Ensure-SandalDockerDaemon
            if (-not (Test-Path -LiteralPath $contentDb)) {
                & (Join-Path $root 'scripts\extract_game_data.ps1')
            }
            & docker compose -f $compose --env-file $envFile up -d --wait --remove-orphans   # (a NATS container left by an older compose file is removed)
            if ($LASTEXITCODE -ne 0) { throw 'docker compose up failed (is Docker running?)' }
            # The init script creates aoc_test only on a fresh volume; make sure it exists.
            $exists = & docker compose -f $compose --env-file $envFile exec -T postgres psql -U aoc -d aoc -tAc "SELECT 1 FROM pg_database WHERE datname = 'aoc_test'"
            if ("$exists".Trim() -ne '1') {
                & docker compose -f $compose --env-file $envFile exec -T postgres psql -U aoc -d aoc -c 'CREATE DATABASE aoc_test'
                if ($LASTEXITCODE -ne 0) { throw 'could not create the aoc_test database' }
            }
        }
        'down' {
            if (-not (Test-Path -LiteralPath $compose)) {
                $null = Sync-SandalDeployStage -RepoRoot $root
            }
            $null = Ensure-SandalDockerDaemon
            & docker compose -f $compose --env-file $envFile down --remove-orphans
            if ($LASTEXITCODE -ne 0) { throw 'docker compose down failed' }
        }
        'test' {
            $savedPath = $env:Path
            $savedTestDb = $env:AOC_TEST_DATABASE_URL
            $savedContentDb = $env:AOC_CONTENT_DB
            $pgPass = if ($env:POSTGRES_PASSWORD) { $env:POSTGRES_PASSWORD } else { '' }
            foreach ($candEnv in @($envFile, (Join-Path $root 'src' 'config' 'sandal.local.env'))) {
                if (-not $pgPass -and (Test-Path -LiteralPath $candEnv -PathType Leaf)) {
                    $m = Get-SandalEnvMap -EnvPath $candEnv
                    if ($m.Contains('POSTGRES_PASSWORD') -and -not [string]::IsNullOrWhiteSpace($m['POSTGRES_PASSWORD'])) { $pgPass = [string]$m['POSTGRES_PASSWORD'] }
                }
            }
            if (-not $pgPass) { $pgPass = 'aoc' }
            $env:AOC_TEST_DATABASE_URL = "postgres://aoc:$pgPass@127.0.0.1:5432/aoc_test?sslmode=disable"
            # The tests of the creation rules read the real content.db when there is one (they skip without).
            if (Test-Path $contentDb) { $env:AOC_CONTENT_DB = $contentDb }
            $env:Path = (Join-Path (& go env GOPATH) 'bin') + $pathSep + $env:Path   # buf and its plugins
            Push-Location $svc
            try {
                & buf lint ../../libs/aocproto/proto/internal
                if ($LASTEXITCODE -ne 0) { throw 'buf lint failed' }
                Ensure-GoProtoGen
                & go vet ./...
                if ($LASTEXITCODE -ne 0) { throw 'go vet failed' }
                & go test -count=1 ./...   # -count=1: DB-backed results are never cached
                if ($LASTEXITCODE -ne 0) { throw 'go test failed' }
            } finally {
                Pop-Location
                $env:Path = $savedPath
                $env:AOC_TEST_DATABASE_URL = $savedTestDb
                $env:AOC_CONTENT_DB = $savedContentDb
            }
        }
        'login' {
            # Signs in as SandalLauncher does and saves the session token
            # for launches that skip the launcher window.
            $creds = Join-Path $root 'src' 'config' 'dev_login.local.json'
            if (-not (Test-Path $creds)) {
                throw "missing ${creds}: create it as {""username"": ""..."", ""password"": ""...""} for an account registered through the launcher"
            }
            # UTF-8 both ways: PowerShell 5.1 reads a file without a BOM as ANSI
            # and sends a string body as ISO-8859-1, which breaks non-ASCII passwords.
            $body = [Text.Encoding]::UTF8.GetBytes((Get-Content -Raw -Encoding UTF8 $creds))
            try {
                $resp = Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:8081/api/v1/auth/login' `
                    -ContentType 'application/json; charset=utf-8' -Body $body
            } catch {
                $response = $_.Exception.Response
                if ($null -eq $response) {
                    throw "login failed: $($_.Exception.Message) (is auth running? scripts\services.ps1 run)"
                }
                $status = [int]$response.StatusCode
                if ($status -eq 401) { throw "login failed: wrong username or password in $creds" }
                throw "login failed: auth answered HTTP ${status}: $($_.Exception.Message)"
            }
            if (-not $resp.access_token) { throw 'login failed: the reply has no access_token' }
            $loginFile = Join-Path $root 'dist' 'Release' 'data' 'ics_auth_token.txt'
            New-Item -ItemType Directory -Force (Split-Path $loginFile) | Out-Null
            [System.IO.File]::WriteAllText($loginFile, $resp.access_token)
            Set-OwnerOnly $loginFile
            Write-Host "signed in as $($resp.username); token saved"
        }
        'build' { Build }
        'run' {
            if (-not (Test-Path $worldExe)) {
                throw "$worldExe is missing: build it with scripts\build.ps1 -Target world"
            }
            if (-not (Test-Path $contentDb)) {
                & (Join-Path $root 'scripts' 'extract_game_data.ps1')
            }
            if (-not (Test-Path $contentDb)) {
                $who = 'the character service will refuse to create characters and world every login'
                Write-Host "warning: $contentDb does not exist: $who. Build it: cd src\server\services; go run ./cmd/contentc (src\server\services\README.md, 'Content')" -ForegroundColor Yellow
            }
            $serverCrt = Join-Path $root 'dist' 'certs' 'server.crt'
            $localhostCrt = Join-Path $root 'dist' 'Release' 'certs' 'localhost.crt'
            if (-not (Test-Path -LiteralPath $serverCrt) -and -not (Test-Path -LiteralPath $localhostCrt)) {
                $null = & (Join-Path $root 'scripts' 'sandal' 'certs.ps1') -OutputDir (Join-Path $root 'dist' 'certs') -RepoRoot $root -Json
            }

            # Check running processes first to prevent compiling locked binaries
            $runningProcs = @(Get-ServiceProcess)

            if ($runningProcs.Count -eq 0) {
                Build
            } else {
                Write-Host "Notice: Skipping rebuild because active services were detected." -ForegroundColor Cyan
            }

            New-ServiceToken
            $started = [System.Collections.Generic.List[object]]::new()
            $names = @('auth', 'character', 'citynode', 'quests', 'mesh', 'gateway', 'world')
            $savedStatic = $env:AOC_MESH_STATIC_WORLD
            $env:AOC_MESH_STATIC_WORLD = ''

            New-Item -ItemType Directory -Force $logs | Out-Null

            try {
                foreach ($name in $names) {
                    $isWorld = ($name -eq 'world')
                    $exe = if ($isWorld) { $worldExe } else { Join-Path $out "$name$exeExt" }

                    $alreadyRunning = $runningProcs | Where-Object { ($_.Path -eq $exe -or ($isWorld -and $_.Name -eq 'world')) -and -not $_.HasExited }
                    if ($alreadyRunning) {
                        Write-Host "Skipping $name (PID $($alreadyRunning.Id) is already running)" -ForegroundColor Yellow
                        continue
                    }

                    $exeArgs = if ($isWorld -or $name -eq 'character') { " --content-db `"$contentDb`"" } else { '' }
                    $log = Join-Path $logs "$name.log"
                    $outLog = Join-Path $logs "$name.out.log"
                    foreach ($f in $log, $outLog) {
                        if (Test-Path $f) { Move-Item -Force $f ($f -replace '\.log$', '.prev.log') }
                    }

                    if ($IsWindows) {
                        $cmdString = "/d /s /c `"`"$exe`"$exeArgs 1>`"$outLog`" 2>`"$log`"`""
                        $startup = New-CimInstance -ClassName Win32_ProcessStartup -Property @{
                            ShowWindow = [uint16]0
                        } -ClientOnly
                        $cimArgs = @{
                            CommandLine               = [string]"`"$env:ComSpec`" $cmdString"
                            CurrentDirectory          = [string]$root
                            ProcessStartupInformation = $startup
                        }
                        $result = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments $cimArgs
                        if ($result.ReturnValue -ne 0) {
                            throw "Failed to start $name (WMI ReturnValue: $($result.ReturnValue))"
                        }
                        $p = Get-Process -Id $result.ProcessId -ErrorAction SilentlyContinue
                    } else {
                        $argList = if ($isWorld -or $name -eq 'character') { @('--content-db', $contentDb) } else { @() }
                        $p = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $root -RedirectStandardOutput $outLog -RedirectStandardError $log -PassThru
                    }
                    $started.Add([pscustomobject]@{ Name = $name; Process = $p; Log = $log })
                }
            } finally {
                $env:AOC_MESH_STATIC_WORLD = $savedStatic
            }

            if ($started.Count -gt 0) {
                Start-Sleep -Seconds 2
                $dead = @($started | Where-Object { $_.Process.HasExited })
                if ($dead.Count -gt 0) {
                    Stop-ServiceProcesses @(Get-ServiceProcess)
                    $why = ($dead | ForEach-Object {
                        '{0} exited with code {1} (see {2})' -f $_.Name, $_.Process.ExitCode, $_.Log
                    }) -join '; '
                    throw "startup failed: $why"
                }
            }

            Write-Host ('running: ' + (Format-Process @(Get-ServiceProcess)) + ". Logs: $logs\<name>.log. Addresses: src\server\services\README.md. Stop: scripts\services.ps1 stop")
        }
        'stop' {
            $procs = @(Get-ServiceProcess)
            if ($procs.Count -eq 0) {
                Write-Host "nothing to stop: no auth, character, citynode, quests, mesh, gateway or world running from this checkout"
            } else {
                $what = Format-Process $procs
                Stop-ServiceProcesses $procs
                Write-Host "stopped $what"
            }
        }
    }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
