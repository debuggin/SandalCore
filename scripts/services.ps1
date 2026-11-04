<#
  Go services helper.
    scripts\services.ps1 up      start PostgreSQL + NATS (docker compose, waits for healthy)
    scripts\services.ps1 test    buf lint + generated-code check, go vet + go test (needs 'up'); the tests of the
                                 character creation rules and of the character service's content.db use
                                 $env:AOC_CONTENT_DB (or dist\content\content.db) when that file exists, and skip without it
    scripts\services.ps1 build   build auth, character, citynode, nodectl, quests, questctl, worldctl, charimport, mesh, gateway into dist\services
    scripts\services.ps1 run     build, then start auth, character, citynode, quests, mesh, gateway and worldd hidden; each logs
                                 to dist\services\logs\<name>.log (the previous run's: <name>.prev.log).  citynode
                                 (Nodes 1) serves the city nodes on :50055 and applies config\seeds\citynode_seed.json once, at its
                                 first start; dist\services\nodectl.exe is its command line (services\README.md, "City nodes").
                                 worldctl.exe (the GM travel tools: players, places, tp, speed) talks to worldd's admin API on
                                 :50056 (services\README.md, "GM travel tools")
                                 worldd (dist\Release\worldd.exe, scripts\build.ps1 -Target worldd) registers with mesh;
                                 there is no static world.  worldd gets
                                 --content-db dist\content\content.db (or $env:AOC_CONTENT_DB): the file
                                 contentc builds; without it worldd starts but refuses every login.
                                 $env:AOC_GAMESTATE_MODE (tier1, tier2, tier3, captured; default tier3) reaches worldd as it is.
                                 The character service gets the same --content-db (plan B1): it checks a new
                                 character's race, gender, class and customization against it; without it
                                 the service starts but refuses to create characters, saying why
    scripts\services.ps1 stop    stop those processes, worldd included (drops the gateway's live
                                 client sessions); needs neither Go nor Docker
    scripts\services.ps1 login   sign in with config\dev_login.local.json and save the token for tether_server
    scripts\services.ps1 down    stop the containers (data volume kept)
  contentc, the content.db builder (plan C1), is not built or started here: see services\README.md.
  run creates dist\services\service.token once: the internal services' shared secret, readable
  by this user only.
#>
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

param(
    [Parameter(Mandatory)] [ValidateSet('up', 'test', 'build', 'run', 'stop', 'login', 'down')] [string] $Action
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$compose = Join-Path $root 'deploy\docker-compose.yml'
$svc = Join-Path $root 'services'
$out = Join-Path $root 'dist\services'
$logs = Join-Path $out 'logs'

function Ensure-GoProtoGen {
    $genDir = Join-Path $root 'build\gen\go'
    New-Item -ItemType Directory -Force -Path $genDir | Out-Null
    $savedPath = $env:Path
    $miseShims = (Join-Path $env:LOCALAPPDATA 'mise\shims')
    $goBin = Join-Path (& go env GOPATH) 'bin'
    $env:Path = "$miseShims;$goBin;$env:USERPROFILE\go\bin;$env:Path"
    Push-Location $svc
    try {
        & buf generate ../libs/aocproto/proto/internal
        if ($LASTEXITCODE -ne 0) { throw 'buf generate ../libs/aocproto/proto/internal failed' }
        if (Test-Path '../libs/aocproto/proto/ics_xclient.proto') {
            & buf generate ../libs/aocproto/proto/ics_xclient.proto
            if ($LASTEXITCODE -ne 0) { throw 'buf generate ../libs/aocproto/proto/ics_xclient.proto failed' }
        }
    } finally {
        Pop-Location
        $env:Path = $savedPath
    }
    $goModPath = Join-Path $genDir 'go.mod'
    $goModContent = "module aocrop/services/gen`n`ngo 1.24.0`n`nrequire (`n`tgoogle.golang.org/grpc v1.79.3`n`tgoogle.golang.org/protobuf v1.36.11`n)`n"
    [System.IO.File]::WriteAllText($goModPath, $goModContent, [System.Text.UTF8Encoding]::new($false))
    $nodeConsts = Join-Path $genDir 'citynodeconsts\constants_gen.go'
    if (-not (Test-Path -LiteralPath $nodeConsts)) {
        $env:PYTHONPYCACHEPREFIX = Join-Path $root 'build\pycache'
        $ini = Join-Path $root 'dist\content\client_config\DefaultAoCRecordConstants.ini'
        if (Test-Path -LiteralPath $ini) {
            & python (Join-Path $root 'tools\schemagen\gen_node_constants.py') --ini $ini
            if ($LASTEXITCODE -ne 0) { throw "gen_node_constants.py failed (exit $LASTEXITCODE)" }
        }
    }
}

function Build {
    Ensure-GoProtoGen
    New-Item -ItemType Directory -Force $out | Out-Null
    Push-Location $svc
    try {
        foreach ($name in 'auth', 'character', 'citynode', 'nodectl', 'quests', 'questctl', 'worldctl', 'charimport', 'mesh', 'gateway') {
            & go build -o (Join-Path $out "$name.exe") "./cmd/$name"
            if ($LASTEXITCODE -ne 0) { throw "go build $name failed" }
        }
    } finally { Pop-Location }
    Write-Host "built into $out"
}
# [C O A + Cyb]

$worldd = Join-Path $root 'dist\Release\worldd.exe'
$tokenFile = Join-Path $out 'service.token'
# content.db, the file worldd values characters from and the character service checks new characters against
# (plan C1's contentc, services\README.md "Content").
$contentDb = if ($env:AOC_CONTENT_DB) { $env:AOC_CONTENT_DB } else { Join-Path $root 'dist\content\content.db' }

# The auth, character, citynode, quests, mesh, gateway and worldd processes running from this
# checkout (not another checkout's copy).
function Get-ServiceProcess {
    $exes = @('auth', 'character', 'citynode', 'quests', 'mesh', 'gateway' | ForEach-Object { Join-Path $out "$_.exe" }) + $worldd
    Get-Process -Name auth, character, citynode, quests, mesh, gateway, worldd -ErrorAction SilentlyContinue |
        Where-Object { $exes -contains $_.Path }
}

# Lets only this user open $path (like the launcher's saved login token).
function Set-OwnerOnly($path) {
    & icacls $path /inheritance:r /grant:r "${env:USERDOMAIN}\${env:USERNAME}:(F)" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "could not restrict $path to ${env:USERNAME}" }
}

# The internal services' shared secret (services/internal/svcauth): 32 random
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
            & docker compose -f $compose up -d --wait
            if ($LASTEXITCODE -ne 0) { throw 'docker compose up failed (is Docker running?)' }
            # The init script creates aoc_test only on a fresh volume; make sure it exists.
            $exists = & docker compose -f $compose exec -T postgres psql -U aoc -d aoc -tAc "SELECT 1 FROM pg_database WHERE datname = 'aoc_test'"
            if ("$exists".Trim() -ne '1') {
                & docker compose -f $compose exec -T postgres psql -U aoc -d aoc -c 'CREATE DATABASE aoc_test'
                if ($LASTEXITCODE -ne 0) { throw 'could not create the aoc_test database' }
            }
        }
        'down' {
            & docker compose -f $compose down
            if ($LASTEXITCODE -ne 0) { throw 'docker compose down failed' }
        }
        'test' {
            $savedPath = $env:Path
            $savedTestDb = $env:AOC_TEST_DATABASE_URL
            $savedContentDb = $env:AOC_CONTENT_DB
            $env:AOC_TEST_DATABASE_URL = 'postgres://aoc:aoc@127.0.0.1:5432/aoc_test?sslmode=disable'
            # The tests of the creation rules read the real content.db when there is one (they skip without).
            if (Test-Path $contentDb) { $env:AOC_CONTENT_DB = $contentDb }
            $env:Path = (Join-Path (& go env GOPATH) 'bin') + ';' + $env:Path   # buf and its plugins
            Push-Location $svc
            try {
                & buf lint ../libs/aocproto/proto/internal
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
            # Signs in as launcher.exe does and saves the token where
            # tether_server reads it, for launches that skip the launcher window.
            $creds = Join-Path $root 'config\dev_login.local.json'
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
            $loginFile = Join-Path $root 'dist\Release\data\ics_auth_token.txt'
            New-Item -ItemType Directory -Force (Split-Path $loginFile) | Out-Null
            [System.IO.File]::WriteAllText($loginFile, $resp.access_token)
            Set-OwnerOnly $loginFile
            Write-Host "signed in as $($resp.username); token saved for tether_server"
        }
        'build' { Build }
        'run' {
            if (-not (Test-Path $worldd)) {
                throw "$worldd is missing: build it with scripts\build.ps1 -Target worldd"
            }
            if (-not (Test-Path $contentDb)) {
                $who = 'the character service will refuse to create characters and worldd every login'
                Write-Host "warning: $contentDb does not exist: $who. Build it: cd services; go run ./cmd/contentc (services\README.md, 'Content')" -ForegroundColor Yellow
            }

            # Check running processes first to prevent compiling locked binaries
            $runningProcs = @(Get-ServiceProcess)

            if ($runningProcs.Count -eq 0) {
                Build
            } else {
                Write-Host "Notice: Skipping rebuild because active services were detected." -ForegroundColor Cyan
            }

            New-ServiceToken
            $started = @()
            $names = @('auth', 'character', 'citynode', 'quests', 'mesh', 'gateway', 'worldd')
            $savedStatic = $env:AOC_MESH_STATIC_WORLD
            $env:AOC_MESH_STATIC_WORLD = ''

            New-Item -ItemType Directory -Force $logs | Out-Null

            # Configured once for silent background execution
            $startup = New-CimInstance -ClassName Win32_ProcessStartup -Property @{
                ShowWindow = 0
            } -ClientOnly

            try {
                foreach ($name in $names) {
                    $exe = if ($name -eq 'worldd') { $worldd } else { Join-Path $out "$name.exe" }

                    $alreadyRunning = $runningProcs | Where-Object { $_.Path -eq $exe -and -not $_.HasExited }
                    if ($alreadyRunning) {
                        Write-Host "Skipping $name (PID $($alreadyRunning.Id) is already running)" -ForegroundColor Yellow
                        continue
                    }

                    $exeArgs = if ($name -eq 'worldd' -or $name -eq 'character') { " --content-db `"$contentDb`"" } else { '' }
                    $log = Join-Path $logs "$name.log"
                    $outLog = Join-Path $logs "$name.out.log"
                    foreach ($f in $log, $outLog) {
                        if (Test-Path $f) { Move-Item -Force $f ($f -replace '\.log$', '.prev.log') }
                    }

                    $cmdString = "/d /s /c `"`"$exe`"$exeArgs 1>`"$outLog`" 2>`"$log`"`""

                    # Define startup configuration specifically as a Win32_ProcessStartup object
                    $startup = New-CimInstance -ClassName Win32_ProcessStartup -Property @{
                        ShowWindow = [uint16]0  # 0 = SW_HIDE, explicitly typed as uint16 for CIM
                    } -ClientOnly

                    # Explicitly cast arguments to strings to prevent CIM Type mismatch
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
                    $started += [pscustomobject]@{ Name = $name; Process = $p; Log = $log }
                }
            } finally {
                $env:AOC_MESH_STATIC_WORLD = $savedStatic
            }

            if ($started.Count -gt 0) {
                Start-Sleep -Seconds 2
                $dead = @($started | Where-Object { $_.Process.HasExited })
                if ($dead.Count -gt 0) {
                    Get-ServiceProcess | Stop-Process -Force -ErrorAction SilentlyContinue
                    $why = ($dead | ForEach-Object {
                        '{0} exited with code {1} (see {2})' -f $_.Name, $_.Process.ExitCode, $_.Log
                    }) -join '; '
                    throw "startup failed: $why"
                }
            }

            Write-Host ('running: ' + (Format-Process @(Get-ServiceProcess)) + ". Logs: $logs\<name>.log. Addresses: services\README.md. Stop: scripts\services.ps1 stop")
        }
        'stop' {
            $procs = @(Get-ServiceProcess)
            if ($procs.Count -eq 0) {
                Write-Host "nothing to stop: no auth, character, citynode, quests, mesh, gateway or worldd running from this checkout"
            } else {
                $what = Format-Process $procs
                $procs | Stop-Process -Force -ErrorAction SilentlyContinue
                $procs | ForEach-Object { $null = $_.WaitForExit(5000) }
                Write-Host "stopped $what"
            }
        }
    }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
