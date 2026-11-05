#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($null -ne (Get-Module Pester) -and (Get-Module Pester).Version.Major -lt 5) {
    function Should {
        if ($args.Count -ge 1 -and "$($args[0])" -match '^-?Throw$') {
            $threw = $false
            foreach ($sbItem in $input) {
                try { & $sbItem *>$null } catch { $threw = $true }
            }
            if (-not $threw) { throw 'Expected expression to throw an exception.' }
            return
        }
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

function Test-SandalHardenedComposeContract {
    param([Parameter(Mandatory)][string]$ComposeYaml)
    $requiredTokens = @(
        'healthcheck:',
        'service_healthy',
        'read_only: true',
        'cap_drop:',
        'ALL',
        'no-new-privileges:true',
        'cpus:',
        'mem_limit:',
        'internal: true'
    )
    foreach ($token in $requiredTokens) {
        if (-not $ComposeYaml.Contains($token)) {
            return $false
        }
    }
    return (-not [regex]::IsMatch($ComposeYaml, '(?m)^\s*-\s*["'']?0\.0\.0\.0:'))
}

Describe 'SandalCore Wave 04.6: Container, Compose, Kubernetes & Systemd Packaging Validation Suite' {
    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
        $script:DeployDir = Join-Path $script:RepoRoot 'src\deploy'
        $script:DockerDir = Join-Path $script:DeployDir 'docker'
        $script:K8sDir = Join-Path $script:DeployDir 'k8s'
        $script:SystemdUnit = Join-Path $script:DeployDir 'systemd\sandal-world@.service'
        $script:ComposeFile = Join-Path $script:DeployDir 'compose.yml'
        $script:MaterializedComposeFile = Join-Path $script:RepoRoot 'deploy\compose.yml'
        $script:MaterializedEnvFile = Join-Path $script:RepoRoot 'deploy\.env'
        $script:EnvoyConfigFile = Join-Path $script:DeployDir 'envoy\envoy.yaml'
        $script:DockerignoreFile = if (Test-Path -LiteralPath (Join-Path $script:DeployDir '.dockerignore')) { Join-Path $script:DeployDir '.dockerignore' } else { Join-Path $script:RepoRoot '.dockerignore' }
    }

    Context '1. Multi-Stage Digest-Pinned Dockerfiles & .dockerignore Hygiene (OPS-01, OPS-02, SEC-01)' {
        It 'Enforces @sha256 digest pinning, non-root USER, exec-form ENTRYPOINT/HEALTHCHECK, and STOPSIGNAL SIGTERM across all 4 Dockerfiles' {
            $expectedUsers = @{
                'cpp.Dockerfile'      = '65532:65532'
                'go.Dockerfile'       = '65532:65532'
                'postgres.Dockerfile' = '70:70'
                'web.Dockerfile'      = '65532:65532'
            }
            foreach ($entry in $expectedUsers.GetEnumerator()) {
                $dfPath = Join-Path $script:DockerDir $entry.Key
                (Test-Path -LiteralPath $dfPath) | Should -BeTrue
                $raw = [System.IO.File]::ReadAllText($dfPath, [System.Text.Encoding]::UTF8)

                $fromLines = @([regex]::Matches($raw, '(?m)^FROM\s+.+$') | ForEach-Object { $_.Value })
                ($fromLines.Count -ge 1) | Should -BeTrue
                foreach ($fromLine in $fromLines) {
                    $fromLine | Should -Match '@sha256:[0-9a-f]{64}\b'
                    ($fromLine -match ':latest\b') | Should -BeFalse
                }

                $raw.Contains("USER $($entry.Value)") | Should -BeTrue
                $raw.Contains("--chown=$($entry.Value)") | Should -BeTrue
                $raw.Contains('STOPSIGNAL SIGTERM') | Should -BeTrue
                $raw | Should -Match '(?m)^ENTRYPOINT\s+\["[^"]+"'
                $raw | Should -Match '(?ms)HEALTHCHECK\s+.*CMD\s+\["[^"]+"'
                $raw.Contains('org.opencontainers.image.source=') | Should -BeTrue
                $raw.Contains('org.opencontainers.image.revision=') | Should -BeTrue
            }
        }

        It 'Validates .dockerignore context isolation excluding VCS, build artifacts, pak archives, and secret keys' {
            (Test-Path -LiteralPath $script:DockerignoreFile) | Should -BeTrue
            $ignoreText = [System.IO.File]::ReadAllText($script:DockerignoreFile, [System.Text.Encoding]::UTF8)
            foreach ($pattern in @('.git', '.beads', 'graphify-out', '_workspace', '.agents', 'build/', 'node_modules/', '*.pdb', '*.pak', '.env', '*.key', 'certs/*.key')) {
                $ignoreText.Contains($pattern) | Should -BeTrue
            }
        }
    }

    Context '2. Compose Stack Readiness, Security Anchors & Network Isolation (OPS-03, SEC-11)' {
        It 'Validates deploy/compose.yml loopback port binding, read-only init mounts, TCP healthcheck, and hardened Compose contract' {
            (Test-Path -LiteralPath $script:ComposeFile) | Should -BeTrue
            $composeRaw = [System.IO.File]::ReadAllText($script:ComposeFile, [System.Text.Encoding]::UTF8)
            $composeRaw.Contains('healthcheck:') | Should -BeTrue
            $composeRaw.Contains('pg_isready -h 127.0.0.1') | Should -BeTrue
            $composeRaw.Contains('127.0.0.1:${INGRESS_HTTP_PORT:-8080}:8080/tcp') | Should -BeTrue
            $composeRaw.Contains('5432:5432') | Should -BeFalse
            $composeRaw.Contains(':ro') | Should -BeTrue
            ($composeRaw -match '0\.0\.0\.0:') | Should -BeFalse
            ($composeRaw -match '(?m)^\s*nats\s*:') | Should -BeFalse

            if ($composeRaw.Contains('service_healthy')) {
                (Test-SandalHardenedComposeContract -ComposeYaml $composeRaw) | Should -BeTrue
            }

            $hardenedFixture = @'
x-hardened: &hardened
  read_only: true
  cap_drop: [ALL]
  security_opt:
    - no-new-privileges:true
  cpus: 2.0
  mem_limit: 2g
services:
  world:
    <<: *hardened
    depends_on:
      postgres:
        condition: service_healthy
    healthcheck:
      test: ["CMD", "/usr/local/bin/service", "--healthcheck", "http://127.0.0.1:9091/livez"]
networks:
  mesh_internal:
    internal: true
'@
            (Test-SandalHardenedComposeContract -ComposeYaml $hardenedFixture) | Should -BeTrue
            (Test-SandalHardenedComposeContract -ComposeYaml ($hardenedFixture.Replace('cap_drop: [ALL]', ''))) | Should -BeFalse
            (Test-SandalHardenedComposeContract -ComposeYaml ($hardenedFixture.Replace('internal: true', 'internal: false'))) | Should -BeFalse
        }
    }

    Context '3. Kubernetes StatefulSet, Stateless Deployments & Zero-Trust NetworkPolicies (K8S-01..K8S-04, SEC-11..SEC-13)' {
        It 'Validates deploy/k8s/world-statefulset.yaml Headless Service, Parallel StatefulSet, Guaranteed QoS, PSS Restricted & PDB' {
            $worldPath = Join-Path $script:K8sDir 'world-statefulset.yaml'
            (Test-Path -LiteralPath $worldPath) | Should -BeTrue
            $raw = [System.IO.File]::ReadAllText($worldPath, [System.Text.Encoding]::UTF8)

            $docs = @($raw -split '(?m)^---\s*$' | Where-Object { $_.Trim().Length -gt 0 })
            $docs.Count | Should -Be 3

            $raw.Contains('kind: Service') | Should -BeTrue
            $raw.Contains('clusterIP: None') | Should -BeTrue
            $raw.Contains('publishNotReadyAddresses: true') | Should -BeTrue
            $raw.Contains('kind: StatefulSet') | Should -BeTrue
            $raw.Contains('serviceName: world') | Should -BeTrue
            $raw.Contains('podManagementPolicy: Parallel') | Should -BeTrue
            $raw.Contains('terminationGracePeriodSeconds: 30') | Should -BeTrue
            $raw.Contains('automountServiceAccountToken: false') | Should -BeTrue

            $raw | Should -Match 'image:\s+ghcr\.io/sandalcore/world@sha256:[0-9a-f]{64}'
            ($raw -match ':latest\b') | Should -BeFalse

            # Guaranteed QoS: requests == limits for CPU (2000m) and memory (4Gi)
            ([regex]::Matches($raw, 'cpu:\s*"2000m"').Count) | Should -Be 2
            ([regex]::Matches($raw, 'memory:\s*"4Gi"').Count) | Should -Be 2

            foreach ($secToken in @(
                'runAsNonRoot: true',
                'runAsUser: 65532',
                'readOnlyRootFilesystem: true',
                'allowPrivilegeEscalation: false',
                'drop: ["ALL"]',
                'type: RuntimeDefault',
                'startupProbe:',
                'livenessProbe:',
                'readinessProbe:',
                'path: /livez',
                'path: /readyz',
                'topologySpreadConstraints:',
                'topology.kubernetes.io/zone',
                'kubernetes.io/hostname',
                'volumeClaimTemplates:',
                'kind: PodDisruptionBudget',
                'maxUnavailable: 1'
            )) {
                $raw.Contains($secToken) | Should -BeTrue
            }
        }

        It 'Validates deploy/k8s/services-deployment.yaml Deployments, Services, PDBs & HPAs v2 for gateway, mesh-gateway, and admin' {
            $svcPath = Join-Path $script:K8sDir 'services-deployment.yaml'
            (Test-Path -LiteralPath $svcPath) | Should -BeTrue
            $raw = [System.IO.File]::ReadAllText($svcPath, [System.Text.Encoding]::UTF8)

            ([regex]::Matches($raw, '(?m)^kind:\s*Deployment\b').Count) | Should -Be 3
            ([regex]::Matches($raw, '(?m)^kind:\s*Service\b').Count) | Should -Be 3
            ([regex]::Matches($raw, '(?m)^kind:\s*PodDisruptionBudget\b').Count) | Should -Be 3
            ([regex]::Matches($raw, '(?m)^kind:\s*HorizontalPodAutoscaler\b').Count) | Should -Be 3
            ([regex]::Matches($raw, '(?m)^apiVersion:\s*autoscaling/v2\b').Count) | Should -Be 3

            foreach ($svcName in @('gateway', 'mesh-gateway', 'admin')) {
                $raw | Should -Match "image:\s+ghcr\.io/sandalcore/${svcName}@sha256:[0-9a-f]{64}"
                $raw.Contains("name: ${svcName}-pdb") | Should -BeTrue
                $raw.Contains("name: ${svcName}-hpa") | Should -BeTrue
            }
            ($raw -match ':latest\b') | Should -BeFalse

            ([regex]::Matches($raw, 'runAsUser:\s*65532').Count -ge 6) | Should -BeTrue
            ([regex]::Matches($raw, 'readOnlyRootFilesystem:\s*true').Count) | Should -Be 3
            ([regex]::Matches($raw, 'allowPrivilegeEscalation:\s*false').Count) | Should -Be 3
            ([regex]::Matches($raw, 'path:\s*/livez').Count) | Should -Be 6
            ([regex]::Matches($raw, 'path:\s*/readyz').Count) | Should -Be 3
            ([regex]::Matches($raw, 'stabilizationWindowSeconds:\s*300').Count) | Should -Be 3
        }

        It 'Validates deploy/k8s/network-policies.yaml default-deny-all baseline and least-privilege ingress/egress rules' {
            $netPolPath = Join-Path $script:K8sDir 'network-policies.yaml'
            (Test-Path -LiteralPath $netPolPath) | Should -BeTrue
            $raw = [System.IO.File]::ReadAllText($netPolPath, [System.Text.Encoding]::UTF8)

            ([regex]::Matches($raw, '(?m)^kind:\s*NetworkPolicy\b').Count) | Should -Be 6
            foreach ($token in @(
                'name: default-deny-all',
                'podSelector: {}',
                '- Ingress',
                '- Egress',
                'name: allow-world-zone-authority',
                'app.kubernetes.io/name: world',
                'port: 7777',
                'port: 7778',
                'port: 50056',
                'name: allow-gateway-control-plane',
                'port: 8000',
                'name: allow-mesh-gateway-datapath',
                'name: allow-admin-console',
                'port: 8090',
                'name: allow-postgres-and-redis-backends',
                'port: 5432',
                'port: 6379',
                'k8s-app: kube-dns',
                'port: 53',
                'egress: []'
            )) {
                $raw.Contains($token) | Should -BeTrue
            }
        }
    }

    Context '4. Bare-Metal Systemd Template Unit Sandboxing & Signal Drain (OPS-04, STD-10)' {
        It 'Validates deploy/systemd/sandal-world@.service sandboxing directives, SIGTERM drain, and LimitNOFILE' {
            (Test-Path -LiteralPath $script:SystemdUnit) | Should -BeTrue
            $unitRaw = [System.IO.File]::ReadAllText($script:SystemdUnit, [System.Text.Encoding]::UTF8)

            foreach ($directive in @(
                '[Unit]',
                'After=network-online.target',
                'Wants=network-online.target',
                '[Service]',
                'Type=exec',
                'User=sandal',
                'Group=sandal',
                'KillSignal=SIGTERM',
                'TimeoutStopSec=30',
                'Restart=on-failure',
                'RestartSec=5s',
                'LimitNOFILE=1048576',
                'NoNewPrivileges=yes',
                'ProtectSystem=strict',
                'ProtectHome=yes',
                'PrivateTmp=yes',
                'PrivateDevices=yes',
                'ProtectKernelTunables=yes',
                'ProtectKernelModules=yes',
                'ProtectControlGroups=yes',
                'RestrictSUIDSGID=yes',
                'LockPersonality=yes',
                'SystemCallArchitectures=native',
                'SystemCallFilter=@system-service',
                '[Install]',
                'WantedBy=multi-user.target'
            )) {
                $unitRaw.Contains($directive) | Should -BeTrue
            }
        }
    }

    Context '5. All 5 Enterprise Dockerfiles, Protobuf Auto-Codegen & Envoy Ingress Proxy (OPS-01..OPS-03, SEC-11)' {
        It 'Enforces @sha256 digest pinning, non-root USER, EXPOSE port/proto annotations, ARG+ENV defaults, OCI LABELs, and exec-form HEALTHCHECK across all 5 Dockerfiles' {
            $dockerfileContracts = @(
                @{
                    Name        = 'cpp.Dockerfile'
                    Users       = @('USER 65532:65532')
                    ExposePorts = @('7777/udp', '50061/tcp')
                    ArgsAndEnvs = @('TARGET_BINARY', 'WORLD_UDP_PORT', 'WORLD_ADMIN_PORT', 'SERVICE_BIND_ADDR')
                },
                @{
                    Name        = 'go.Dockerfile'
                    Users       = @('USER 65532:65532')
                    ExposePorts = @('8081/tcp', '8082/tcp', '50050/tcp', '50052/tcp', '50053/tcp', '50054/tcp', '50056/tcp', '50057/tcp')
                    ArgsAndEnvs = @('SERVICE_NAME', 'SERVICE_HTTP_PORT', 'SERVICE_GRPC_PORT', 'SERVICE_BIND_ADDR', 'AOC_ENV')
                },
                @{
                    Name        = 'postgres.Dockerfile'
                    Users       = @('USER postgres', 'USER 70:70')
                    ExposePorts = @('5432/tcp')
                    ArgsAndEnvs = @('PGPORT', 'POSTGRES_USER', 'POSTGRES_DB')
                },
                @{
                    Name        = 'web.Dockerfile'
                    Users       = @('USER 65532:65532')
                    ExposePorts = @('8088/tcp')
                    ArgsAndEnvs = @('WEB_PORT', 'WEB_BIND_ADDR')
                },
                @{
                    Name        = 'envoy.Dockerfile'
                    Users       = @('USER 65532:65532')
                    ExposePorts = @('8080/tcp', '9090/tcp', '9901/tcp')
                    ArgsAndEnvs = @('ENVOY_HTTP_INGRESS_PORT', 'ENVOY_GRPC_INGRESS_PORT', 'ENVOY_ADMIN_PORT')
                }
            )

            foreach ($spec in $dockerfileContracts) {
                $dfPath = Join-Path $script:DockerDir $spec.Name
                (Test-Path -LiteralPath $dfPath) | Should -BeTrue
                $raw = [System.IO.File]::ReadAllText($dfPath, [System.Text.Encoding]::UTF8)

                $fromLines = @([regex]::Matches($raw, '(?m)^FROM\s+.+$') | ForEach-Object { $_.Value })
                ($fromLines.Count -ge 1) | Should -BeTrue
                foreach ($fromLine in $fromLines) {
                    $fromLine | Should -Match '@sha256:[0-9a-f]{64}\b'
                    ($fromLine -match ':latest(?!@sha256:)\b') | Should -BeFalse
                }

                foreach ($userDir in $spec.Users) {
                    $raw.Contains($userDir) | Should -BeTrue
                }
                ($raw -match '(?m)^USER\s+(?:0|root)\b') | Should -BeFalse

                $exposeLine = ([regex]::Matches($raw, '(?m)^EXPOSE\s+.+$') | ForEach-Object { $_.Value }) -join ' '
                ($exposeLine.Length -gt 0) | Should -BeTrue
                foreach ($portProto in $spec.ExposePorts) {
                    $exposeLine.Contains($portProto) | Should -BeTrue
                }

                foreach ($paramName in $spec.ArgsAndEnvs) {
                    $paramPattern = $paramName -replace '^WORLD_', 'WORLD[A-Z]*_'
                    $raw | Should -Match "(?m)^ARG\s+${paramPattern}\b"
                    $raw | Should -Match "\b${paramPattern}="
                }

                $raw.Contains('org.opencontainers.image.source=') | Should -BeTrue
                $raw.Contains('org.opencontainers.image.revision=') | Should -BeTrue
                ($raw.Contains('org.opencontainers.image.licenses=') -or $raw.Contains('org.opencontainers.image.description=')) | Should -BeTrue
                $raw | Should -Match '(?ms)HEALTHCHECK\s+.*CMD\s+\["[^"]+"'
            }
        }

        It 'Verifies go.Dockerfile auto-generates build/gen/go and constants_gen.go when missing, and cpp.Dockerfile auto-generates build/gen/proto via CMake' {
            $goDf = [System.IO.File]::ReadAllText((Join-Path $script:DockerDir 'go.Dockerfile'), [System.Text.Encoding]::UTF8)
            $goDf.Contains('/workspace/build/gen/go/aoc/proto/v1') | Should -BeTrue
            $goDf.Contains('--go_out=/workspace/build/gen/go') | Should -BeTrue
            $goDf.Contains('--go-grpc_out=/workspace/build/gen/go') | Should -BeTrue
            $goDf.Contains('/workspace/build/gen/go/citynodeconsts/constants_gen.go') | Should -BeTrue
            $goDf.Contains('gen-node-constants.sh') | Should -BeTrue

            $cppDf = [System.IO.File]::ReadAllText((Join-Path $script:DockerDir 'cpp.Dockerfile'), [System.Text.Encoding]::UTF8)
            $cppDf.Contains('cmake -S src -B build/linux') | Should -BeTrue
            $cppDf.Contains('-DAOC_PROTO_GEN_DIR=/workspace/build/gen/proto') | Should -BeTrue
            $cppDf.Contains('gen-node-constants.sh') | Should -BeTrue
        }

        It 'Validates src/deploy/envoy/envoy.yaml ingress_http (0.0.0.0:8080 + WebSocket), ingress_grpc (0.0.0.0:9090 h2c + 7 gRPC packages), circuit breakers, and active health checks' {
            (Test-Path -LiteralPath $script:EnvoyConfigFile) | Should -BeTrue
            $envoyRaw = [System.IO.File]::ReadAllText($script:EnvoyConfigFile, [System.Text.Encoding]::UTF8)

            foreach ($token in @(
                'name: ingress_http',
                'address: 0.0.0.0',
                'port_value: 8080',
                'upgrade_type: websocket',
                'prefix: "/api/v1/auth/"',
                'prefix: "/api/v1/catalog/"',
                'prefix: "/api/v1/world/"',
                'prefix: "/api/v1/gm/"',
                'prefix: "/"',
                'name: ingress_grpc',
                'port_value: 9090',
                'max_concurrent_streams: 1024',
                'prefix: "/aoc.auth.v1."',
                'prefix: "/aoc.gateway.v1."',
                'prefix: "/aoc.character.v1."',
                'prefix: "/aoc.mesh.v1."',
                'prefix: "/aoc.citynode.v1."',
                'prefix: "/aoc.quest.v1."',
                'prefix: "/aoc.worldadmin.v1."',
                'cluster: world_admin_grpc',
                'circuit_breakers:',
                'max_connections: 10000',
                'health_checks:',
                'tcp_health_check: {}',
                'grpc_health_check: {}'
            )) {
                $envoyRaw.Contains($token) | Should -BeTrue
            }
            $envoyRaw.Contains('ingress_tether_tcp') | Should -BeFalse
            $envoyRaw.Contains('tether_tcp') | Should -BeFalse
        }
    }

    Context '6. 11-Service Compose Topology, 3-Zone Network Isolation & Docker Compose Config Validation (OPS-03, SEC-11)' {
        It 'Validates all 11 services, 3-zone network isolation (edge_net, internal service_net, internal data_net), cap_drop/no-new-privileges/read_only/tmpfs hardening, 127.0.0.1 launcher ports, and docker compose config --quiet' {
            $composePaths = @($script:ComposeFile)
            if ((Test-Path -LiteralPath $script:MaterializedComposeFile) -and ([System.IO.File]::ReadAllText($script:MaterializedComposeFile, [System.Text.Encoding]::UTF8).Contains('WORLD_UDP_PORT'))) {
                $composePaths += $script:MaterializedComposeFile
            }

            $expectedServices = @(
                'postgres', 'auth', 'character', 'citynode', 'quest', 'mesh',
                'gateway', 'mesh-gateway', 'web-admin', 'world', 'ingress-proxy'
            )
            $excludedFromDataNet = @('ingress-proxy', 'web-admin', 'world')
            $requiredOnDataNet = @('postgres', 'auth', 'character', 'citynode', 'quest', 'gateway', 'mesh-gateway')

            foreach ($cPath in $composePaths) {
                $raw = [System.IO.File]::ReadAllText($cPath, [System.Text.Encoding]::UTF8)
                $raw.Contains('aoc-tether') | Should -BeFalse
                ($raw -match '(?m)^  tether:\s*$') | Should -BeFalse
                $raw.Contains('GRPC_ENFORCE_ALPN_ENABLED: "false"') | Should -BeTrue

                $anchorMatch = [regex]::Match($raw, '(?ms)^x-hardened:\s*&hardened\r?\n(.*?)(?=^services:\r?\n)')
                $anchorMatch.Success | Should -BeTrue
                $anchorBlock = $anchorMatch.Groups[1].Value
                $anchorBlock.Contains('read_only: true') | Should -BeTrue
                $anchorBlock.Contains('cap_drop:') | Should -BeTrue
                $anchorBlock.Contains('- ALL') | Should -BeTrue
                $anchorBlock.Contains('no-new-privileges:true') | Should -BeTrue
                $anchorBlock.Contains('tmpfs:') | Should -BeTrue

                foreach ($svc in $expectedServices) {
                    $svcMatch = [regex]::Match($raw, "(?ms)^  $([regex]::Escape($svc)):\r?\n(.*?)(?=^  [a-z0-9_-]+:\r?\n|^networks:\r?\n|^volumes:\r?\n)")
                    $svcMatch.Success | Should -BeTrue
                    $svcBlock = $svcMatch.Groups[1].Value
                    $svcBlock.Contains('container_name: aoc-') | Should -BeTrue
                    $svcBlock | Should -Match '(?m)^\s*container_name:\s*aoc-[a-z0-9_-]+\s*$'
                    $svcBlock | Should -Match "(?m)^\s*hostname:\s*$([regex]::Escape($svc))\s*$"
                    $svcBlock | Should -Match '(?m)^\s*image:\s*sandal-[a-z0-9_-]+:local\s*$'
                    $usesHardenedAnchor = $svcBlock.Contains('<<: *hardened')
                    if (-not $usesHardenedAnchor) {
                        $svcBlock.Contains('read_only: true') | Should -BeTrue
                        $svcBlock.Contains('cap_drop:') | Should -BeTrue
                        $svcBlock.Contains('- ALL') | Should -BeTrue
                        $svcBlock.Contains('no-new-privileges:true') | Should -BeTrue
                        $svcBlock.Contains('tmpfs:') | Should -BeTrue
                    } else {
                        $usesHardenedAnchor | Should -BeTrue
                    }

                    if ($excludedFromDataNet -contains $svc) {
                        $svcBlock.Contains('- data_net') | Should -BeFalse
                    }
                    if ($requiredOnDataNet -contains $svc) {
                        $svcBlock.Contains('- data_net') | Should -BeTrue
                    }
                    if ($svc -in @('ingress-proxy', 'world')) {
                        $svcBlock.Contains('ports:') | Should -BeTrue
                        $svcBlock.Contains('- edge_net') | Should -BeTrue
                    } else {
                        $svcBlock.Contains('ports:') | Should -BeFalse
                        $svcBlock.Contains('- edge_net') | Should -BeFalse
                    }
                }

                $netSection = [regex]::Match($raw, '(?ms)^networks:\r?\n(.*?)(?=^volumes:\r?\n|\z)').Groups[1].Value
                $netSection.Contains('edge_net:') | Should -BeTrue
                $netSection | Should -Match '(?ms)service_net:\r?\n\s+driver:\s*bridge\r?\n\s+internal:\s*true'
                $netSection | Should -Match '(?ms)data_net:\r?\n\s+driver:\s*bridge\r?\n\s+internal:\s*true'

                foreach ($portBinding in @(
                    '127.0.0.1:${INGRESS_HTTP_PORT:-8080}:8080/tcp',
                    '127.0.0.1:${INGRESS_GRPC_PORT:-9090}:9090/tcp',
                    '127.0.0.1:${GATEWAY_TLS_PORT:-50061}:50061/tcp',
                    '127.0.0.1:${WORLD_UDP_PORT:-7777}:7777/udp'
                )) {
                    $raw.Contains($portBinding) | Should -BeTrue
                }
                $raw.Contains('50051:50051') | Should -BeFalse
                $raw.Contains('5432:5432') | Should -BeFalse
                $raw.Contains('9901:9901') | Should -BeFalse
            }

            if ($null -ne (Get-Command docker -ErrorAction SilentlyContinue)) {
                if ((Test-Path -LiteralPath $script:MaterializedComposeFile) -and (Test-Path -LiteralPath $script:MaterializedEnvFile)) {
                    & docker compose -f $script:MaterializedComposeFile --env-file $script:MaterializedEnvFile config --quiet
                    $LASTEXITCODE | Should -Be 0
                }
                $runningRows = @(& docker ps --format "{{.Names}}|{{.Image}}" 2>$null | Where-Object { $_ -match '^aoc-' })
                if ($LASTEXITCODE -eq 0 -and $runningRows.Count -gt 0) {
                    foreach ($row in $runningRows) {
                        $parts = $row -split '\|', 2
                        $cName = $parts[0].Trim()
                        $cImage = $parts[1].Trim()
                        ($cName -match '-\d+$') | Should -BeFalse
                        $cName | Should -Match '^aoc-[a-z0-9_-]+$'
                        ($cImage -match '^[0-9a-f]{12}$') | Should -BeFalse
                        $cImage | Should -Match '^sandal-[a-z0-9_-]+:local$'
                    }
                }
            }
        }
    }
}
