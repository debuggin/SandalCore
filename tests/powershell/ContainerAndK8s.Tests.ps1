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
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
        $script:DeployDir = Join-Path $script:RepoRoot 'deploy'
        $script:DockerDir = Join-Path $script:DeployDir 'docker'
        $script:K8sDir = Join-Path $script:DeployDir 'k8s'
        $script:SystemdUnit = Join-Path $script:DeployDir 'systemd\sandal-worldd@.service'
        $script:ComposeFile = Join-Path $script:DeployDir 'compose.yml'
        $script:DockerignoreFile = Join-Path $script:RepoRoot '.dockerignore'
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
            $composeRaw.Contains('127.0.0.1:5432:5432') | Should -BeTrue
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
  worldd:
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
        It 'Validates deploy/k8s/worldd-statefulset.yaml Headless Service, Parallel StatefulSet, Guaranteed QoS, PSS Restricted & PDB' {
            $worlddPath = Join-Path $script:K8sDir 'worldd-statefulset.yaml'
            (Test-Path -LiteralPath $worlddPath) | Should -BeTrue
            $raw = [System.IO.File]::ReadAllText($worlddPath, [System.Text.Encoding]::UTF8)

            $docs = @($raw -split '(?m)^---\s*$' | Where-Object { $_.Trim().Length -gt 0 })
            $docs.Count | Should -Be 3

            $raw.Contains('kind: Service') | Should -BeTrue
            $raw.Contains('clusterIP: None') | Should -BeTrue
            $raw.Contains('publishNotReadyAddresses: true') | Should -BeTrue
            $raw.Contains('kind: StatefulSet') | Should -BeTrue
            $raw.Contains('serviceName: worldd') | Should -BeTrue
            $raw.Contains('podManagementPolicy: Parallel') | Should -BeTrue
            $raw.Contains('terminationGracePeriodSeconds: 30') | Should -BeTrue
            $raw.Contains('automountServiceAccountToken: false') | Should -BeTrue

            $raw | Should -Match 'image:\s+ghcr\.io/sandalcore/worldd@sha256:[0-9a-f]{64}'
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

        It 'Validates deploy/k8s/services-deployment.yaml Deployments, Services, PDBs & HPAs v2 for gateway, meshgw, and admin' {
            $svcPath = Join-Path $script:K8sDir 'services-deployment.yaml'
            (Test-Path -LiteralPath $svcPath) | Should -BeTrue
            $raw = [System.IO.File]::ReadAllText($svcPath, [System.Text.Encoding]::UTF8)

            ([regex]::Matches($raw, '(?m)^kind:\s*Deployment\b').Count) | Should -Be 3
            ([regex]::Matches($raw, '(?m)^kind:\s*Service\b').Count) | Should -Be 3
            ([regex]::Matches($raw, '(?m)^kind:\s*PodDisruptionBudget\b').Count) | Should -Be 3
            ([regex]::Matches($raw, '(?m)^kind:\s*HorizontalPodAutoscaler\b').Count) | Should -Be 3
            ([regex]::Matches($raw, '(?m)^apiVersion:\s*autoscaling/v2\b').Count) | Should -Be 3

            foreach ($svcName in @('gateway', 'meshgw', 'admin')) {
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
                'name: allow-worldd-zone-authority',
                'port: 7777',
                'port: 7778',
                'port: 50056',
                'name: allow-gateway-control-plane',
                'port: 8000',
                'name: allow-meshgw-datapath',
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
        It 'Validates deploy/systemd/sandal-worldd@.service sandboxing directives, SIGTERM drain, and LimitNOFILE' {
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
}
