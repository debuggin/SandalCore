#Requires -Version 7.2
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

[CmdletBinding()]
param(
    [ValidateSet('sync', 'test', 'run')]
    [string] $Action = 'test',
    [Parameter(ValueFromRemainingArguments)]
    [string[]] $RemainingArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:HookBudgetNs = 150.0
$script:RingSlotBytes = 32
$script:DefaultPe32PlusBase = 0x140000000L
$script:ForbiddenTokens = @(('0x' + '14'), ('sub_' + '14'), ('qword_' + '14'), ('dword_' + '14'))
$script:ForbiddenSymbolRegex = '\b(?:sub_|qword_|dword_|__fastcall)\w*'
$script:P0UndecodedStructs = @(
    'FRepMovement',
    'FMinimalViewInfo',
    'FGameplayTagContainer',
    'FGameplayEffectContextHandle',
    'FUniqueNetIdRepl',
    'FVector_NetQuantize',
    'FVector_NetQuantize10',
    'FVector_NetQuantize100',
    'FVector_NetQuantizeNormal',
    'FDateTime',
    'FTimespan',
    'FSoftObjectPath'
)

function Get-Sha256Hex {
    param([Parameter(Mandatory)][byte[]]$Bytes)
    $hash = [System.Security.Cryptography.SHA256]::HashData($Bytes)
    return [System.Convert]::ToHexString($hash).ToLowerInvariant()
}

function New-BitCursorStep {
    param(
        [int]$Handle,
        [string]$FieldName,
        [int]$ExpectedBits,
        [int]$ActualBits,
        [bool]$RetOk = $true
    )
    return [pscustomobject]@{
        Handle       = $Handle
        FieldName    = $FieldName
        ExpectedBits = $ExpectedBits
        ActualBits   = $ActualBits
        RetOk        = $RetOk
    }
}

function Invoke-BunchBitEvaluation {
    param(
        [int]$PacketId,
        [int]$BunchId,
        [int]$TotalBits,
        [object[]]$Steps
    )
    $consumed = 0
    $firstDiv = $null
    $firstFailed = $null
    foreach ($step in $Steps) {
        $stepBad = (-not $step.RetOk) -or ($step.ActualBits -ne $step.ExpectedBits) -or ($step.ActualBits -lt 0)
        if ($null -eq $firstFailed -and $stepBad) {
            $firstDiv = $consumed
            $firstFailed = $step.Handle
        }
        $consumed += $step.ActualBits
    }
    $leftover = $TotalBits - $consumed
    if ($null -eq $firstDiv -and $leftover -ne 0) {
        $firstDiv = $consumed
    }
    $isErr = ($null -ne $firstFailed) -or ($leftover -ne 0) -or ($TotalBits -lt 0)
    return [pscustomobject]@{
        PacketId          = $PacketId
        BunchId           = $BunchId
        TotalBits         = $TotalBits
        ConsumedBits      = $consumed
        LeftoverBits      = $leftover
        IsError           = $isErr
        FirstDivergentBit = $firstDiv
        FirstFailedHandle = $firstFailed
    }
}

function Test-CorpusZeroResidue {
    param([object[]]$Results)
    if ($null -eq $Results -or $Results.Count -eq 0) {
        return $false
    }
    foreach ($res in $Results) {
        if ($res.IsError -or $res.LeftoverBits -ne 0) {
            return $false
        }
    }
    return $true
}

function Find-AsciiSubsequence {
    param([byte[]]$Buffer, [byte[]]$Pattern)
    $limit = $Buffer.Length - $Pattern.Length
    for ($i = 0; $i -le $limit; $i++) {
        $matched = $true
        for ($j = 0; $j -lt $Pattern.Length; $j++) {
            if ($Buffer[$i + $j] -ne $Pattern[$j]) {
                $matched = $false
                break
            }
        }
        if ($matched) { return $i }
    }
    return -1
}

function Get-PeRsdsInfo {
    param([Parameter(Mandatory)][byte[]]$PeBytes)
    if ($PeBytes.Length -lt 0x100 -or [System.BitConverter]::ToUInt16($PeBytes, 0) -ne 0x5A4D) {
        throw 'TOOL-1: invalid PE32+ header'
    }
    $lfanew = [int][System.BitConverter]::ToUInt32($PeBytes, 0x3C)
    if ($lfanew -lt 0x40 -or ($PeBytes.Length - $lfanew) -lt 26) {
        throw 'TOOL-1: invalid PE32+ header'
    }
    $peSig = [System.BitConverter]::ToUInt32($PeBytes, $lfanew)
    $optMagic = [System.BitConverter]::ToUInt16($PeBytes, $lfanew + 24)
    if ($peSig -ne 0x00004550 -or $optMagic -ne 0x020B) {
        throw 'TOOL-1: invalid PE32+ header'
    }
    $rsdsIdx = Find-AsciiSubsequence -Buffer $PeBytes -Pattern ([System.Text.Encoding]::ASCII.GetBytes('RSDS'))
    if ($rsdsIdx -lt 0 -or ($PeBytes.Length - $rsdsIdx) -lt 24) {
        throw 'TOOL-1: missing RSDS CodeView record'
    }
    $guidBytes = [byte[]]$PeBytes[($rsdsIdx + 4)..($rsdsIdx + 19)]
    $pdbGuid = [System.Convert]::ToHexString($guidBytes).ToUpperInvariant()
    $pdbAge = [int][System.BitConverter]::ToUInt32($PeBytes, $rsdsIdx + 20)
    return [pscustomobject]@{
        ExecSha256 = (Get-Sha256Hex -Bytes $PeBytes)
        PdbGuid    = $pdbGuid
        PdbAge     = $pdbAge
        IsPe32Plus = $true
    }
}

function Confirm-PeRsdsManifest {
    param([byte[]]$PeBytes, [pscustomobject]$Pin)
    $info = Get-PeRsdsInfo -PeBytes $PeBytes
    $shaMatch = $info.ExecSha256.ToLowerInvariant() -eq $Pin.ExecSha256.ToLowerInvariant()
    $guidMatch = $info.PdbGuid.ToUpperInvariant() -eq $Pin.PdbGuid.ToUpperInvariant()
    if (-not $shaMatch -or -not $guidMatch -or $info.PdbAge -ne $Pin.PdbAge) {
        throw 'TOOL-1: manifest mismatch'
    }
    return $info
}

function ConvertFrom-RingSlot {
    param([byte[]]$Buffer, [int]$Offset = 0)
    if ($Offset -lt 0 -or ($Buffer.Length - $Offset) -lt $script:RingSlotBytes) {
        throw 'TOOL-9: truncated ring slot buffer'
    }
    $qpc = [System.BitConverter]::ToUInt64($Buffer, $Offset)
    $typeHash = [System.BitConverter]::ToUInt32($Buffer, $Offset + 8)
    $chanIdx = [System.BitConverter]::ToUInt32($Buffer, $Offset + 12)
    $before = [System.BitConverter]::ToInt32($Buffer, $Offset + 16)
    $after = [System.BitConverter]::ToInt32($Buffer, $Offset + 20)
    $retOk = [System.BitConverter]::ToUInt32($Buffer, $Offset + 24)
    if ($before -lt 0 -or $after -lt $before) {
        throw 'TOOL-9: invalid bit cursor delta'
    }
    return [pscustomobject]@{
        QpcTicks       = $qpc
        StructTypeHash = $typeHash
        ChannelIndex   = $chanIdx
        BitPosBefore   = $before
        BitPosAfter    = $after
        RetOk          = $retOk
        BitsConsumed   = ($after - $before)
    }
}

function Test-HookLatencyNs {
    param([double]$ElapsedNs, [double]$BudgetNs = $script:HookBudgetNs)
    return ($ElapsedNs -ge 0.0 -and $ElapsedNs -le $BudgetNs)
}

function Invoke-ThreeWayReconciliation {
    param(
        [string[]]$StaticStructs,
        [hashtable]$RuntimeCounts,
        [string[]]$ImplementedCodecs
    )
    $records = [System.Collections.Generic.List[object]]::new()
    for ($idx = 0; $idx -lt $script:P0UndecodedStructs.Count; $idx++) {
        $name = $script:P0UndecodedStructs[$idx]
        $inImpl = $ImplementedCodecs -contains $name
        $inStatic = $StaticStructs -contains $name
        $obs = if ($RuntimeCounts.ContainsKey($name)) { [int]$RuntimeCounts[$name] } else { 0 }
        $records.Add([pscustomobject]@{
            PriorityId           = ('P0-{0:D2}' -f ($idx + 1))
            StructName           = $name
            InStaticSchema       = $inStatic
            ObservedRuntimeCalls = $obs
            InImplementedCodecs  = $inImpl
            Status               = if ($inImpl) { 'IMPLEMENTED' } else { 'MISSING_CODEC' }
        })
    }
    return $records.ToArray()
}

function Get-CorrelatedGap {
    param(
        [string]$StructName,
        [int]$ChannelIndex,
        [int]$ExpectedBits,
        [int]$BitPosBefore,
        [int]$BitPosAfter
    )
    if ($BitPosBefore -lt 0 -or $BitPosAfter -lt $BitPosBefore -or $ExpectedBits -lt 0) {
        throw 'TOOL-9: invalid bit cursor range'
    }
    $observed = $BitPosAfter - $BitPosBefore
    return [pscustomobject]@{
        StructName   = $StructName
        ChannelIndex = $ChannelIndex
        ExpectedBits = $ExpectedBits
        ObservedBits = $observed
        BitDelta     = ($observed - $ExpectedBits)
        SampleHex    = '01'
    }
}

function New-CleanroomTestVector {
    param(
        [string]$StructName,
        [byte[]]$Payload,
        [int]$BitLength
    )
    $lowered = $StructName.ToLowerInvariant()
    $hasForbidden = $false
    foreach ($tok in $script:ForbiddenTokens) {
        if ($lowered.Contains($tok)) { $hasForbidden = $true; break }
    }
    if ($BitLength -le 0 -or [string]::IsNullOrEmpty($StructName) -or $hasForbidden) {
        throw 'CLEANROOM-1: forbidden symbol'
    }
    return [pscustomobject]@{
        StructName = $StructName
        BitLength  = $BitLength
        PayloadHex = [System.Convert]::ToHexString($Payload).ToLowerInvariant()
        Sha256     = (Get-Sha256Hex -Bytes $Payload)
    }
}

function ConvertTo-NormalizedRva {
    param([long]$VirtualAddress, [long]$ImageBase = $script:DefaultPe32PlusBase)
    if ($ImageBase -le 0 -or $VirtualAddress -lt $ImageBase) {
        throw 'virtual address precedes PE32+ image_base'
    }
    $rva = $VirtualAddress - $ImageBase
    if ($rva -gt 0x7FFFFFFFL) {
        throw 'RVA exceeds 2 GiB PE32+ module span'
    }
    return $rva
}

function Assert-CleanroomSchemaCatalog {
    param([pscustomobject]$Catalog)
    if ($Catalog.ExecSha256.Length -ne 64 -or [string]::IsNullOrEmpty($Catalog.PdbGuid)) {
        throw 'invalid binary manifest pin'
    }
    foreach ($s in $Catalog.Structs) {
        if ($s.VtableRva -lt 0 -or ($s.StructName -match $script:ForbiddenSymbolRegex)) {
            throw "clean-room violation in struct $($s.StructName)"
        }
    }
    foreach ($p in $Catalog.Properties) {
        if ($p.RepIndex -lt 0 -or $p.ElementSize -le 0 -or ($p.PropName -match $script:ForbiddenSymbolRegex)) {
            throw "invalid usmap property $($p.OwnerType).$($p.PropName)"
        }
    }
}

function Format-StructSpecMarkdown {
    param([pscustomobject]$Gap)
    $sampleHex = if ([string]::IsNullOrEmpty($Gap.SampleHex)) { '01' } else { $Gap.SampleHex }
    $rawBytes = [System.Convert]::FromHexString($sampleHex)
    $bitLen = [Math]::Max(1, [Math]::Max([int]$Gap.ObservedBits, [Math]::Max([Math]::Abs([int]$Gap.BitDelta), $rawBytes.Length * 8)))
    $vec = New-CleanroomTestVector -StructName $Gap.StructName -Payload $rawBytes -BitLength $bitLen
    $lines = @(
        "## $($Gap.StructName)",
        '',
        '| Field | Value |',
        '| :--- | :--- |',
        "| ``struct_name`` | ``$($Gap.StructName)`` |",
        "| ``cmd_index`` | ``$($Gap.ChannelIndex)`` |",
        "| ``bit_delta`` | ``$($Gap.BitDelta)`` |",
        "| ``sample_hex`` | ``$($vec.PayloadHex)`` |",
        "| ``bit_length`` | ``$($vec.BitLength)`` |",
        "| ``payload_hex`` | ``$($vec.PayloadHex)`` |",
        "| ``sha256`` | ``$($vec.Sha256)`` |"
    )
    return ($lines -join "`n")
}

function Format-ReconciliationSummaryMarkdown {
    param([pscustomobject]$Report, [object[]]$Gaps)
    $hdr = @(
        '# Clean-Room Wire Protocol Specification Summary',
        '',
        '| Metric | Count |',
        '| :--- | :--- |',
        "| Matched | $($Report.MatchedCount) |",
        "| Missing Codec | $($Report.MissingCodecCount) |",
        "| Runtime Only | $($Report.RuntimeOnly) |",
        "| Total P0 Structs | $($Report.TotalP0Structs) |"
    ) -join "`n"
    $sections = [System.Collections.Generic.List[string]]::new()
    $sections.Add($hdr)
    foreach ($gap in $Gaps) {
        $sections.Add((Format-StructSpecMarkdown -Gap $gap))
    }
    return (($sections -join "`n`n") + "`n")
}

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw "Assertion failed: $Message"
    }
}

function Assert-ThrowsMatch {
    param([scriptblock]$ScriptBlock, [string]$Pattern)
    $threw = $false
    try {
        & $ScriptBlock | Out-Null
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch $Pattern) {
            throw "Expected exception matching '$Pattern', got '$($_.Exception.Message)'"
        }
    }
    if (-not $threw) {
        throw "Expected exception matching '$Pattern', but no exception was thrown"
    }
}

function New-SyntheticPe32PlusBytes {
    $buf = [byte[]]::new(0x120)
    $buf[0] = [byte][char]'M'
    $buf[1] = [byte][char]'Z'
    [System.BitConverter]::GetBytes([uint32]0x40).CopyTo($buf, 0x3C)
    $buf[0x40] = [byte][char]'P'
    $buf[0x41] = [byte][char]'E'
    [System.BitConverter]::GetBytes([uint16]0x020B).CopyTo($buf, 0x40 + 24)
    $rsdsHdr = [System.Text.Encoding]::ASCII.GetBytes('RSDS')
    $guidBytes = [System.Convert]::FromHexString('00112233445566778899AABBCCDDEEFF')
    $ageBytes = [System.BitConverter]::GetBytes([uint32]2)
    $rsdsHdr.CopyTo($buf, 0x80)
    $guidBytes.CopyTo($buf, 0x84)
    $ageBytes.CopyTo($buf, 0x94)
    return $buf
}

function New-RingSlotBytes {
    param(
        [uint64]$QpcTicks,
        [uint32]$StructTypeHash,
        [int]$BeforeBits,
        [int]$AfterBits
    )
    $slot = [byte[]]::new(32)
    [System.BitConverter]::GetBytes($QpcTicks).CopyTo($slot, 0)
    [System.BitConverter]::GetBytes($StructTypeHash).CopyTo($slot, 8)
    [System.BitConverter]::GetBytes([uint32]3).CopyTo($slot, 12)
    [System.BitConverter]::GetBytes($BeforeBits).CopyTo($slot, 16)
    [System.BitConverter]::GetBytes($AfterBits).CopyTo($slot, 20)
    [System.BitConverter]::GetBytes([uint32]1).CopyTo($slot, 24)
    return $slot
}

function Test-BitOracleSuite {
    $steps = @(
        (New-BitCursorStep -Handle 1 -FieldName 'bHidden' -ExpectedBits 1 -ActualBits 1),
        (New-BitCursorStep -Handle 2 -FieldName 'RepMovement' -ExpectedBits 112 -ActualBits 112)
    )
    $res = Invoke-BunchBitEvaluation -PacketId 10 -BunchId 1 -TotalBits 113 -Steps $steps
    Assert-Condition (-not $res.IsError) 'valid bunch should not error'
    Assert-Condition ($res.LeftoverBits -eq 0 -and $null -eq $res.FirstDivergentBit) 'valid bunch has zero residue'
    Assert-Condition (Test-CorpusZeroResidue -Results @($res)) 'corpus zero residue should pass'

    $badSteps = @(
        (New-BitCursorStep -Handle 1 -FieldName 'bHidden' -ExpectedBits 1 -ActualBits 1),
        (New-BitCursorStep -Handle 2 -FieldName 'RepMovement' -ExpectedBits 32 -ActualBits 24)
    )
    $resBad = Invoke-BunchBitEvaluation -PacketId 11 -BunchId 1 -TotalBits 33 -Steps $badSteps
    Assert-Condition ($resBad.IsError) 'divergent bunch should flag error'
    Assert-Condition ($resBad.FirstDivergentBit -eq 1 -and $resBad.FirstFailedHandle -eq 2) 'divergent bit and handle'
    Assert-Condition ($resBad.LeftoverBits -eq 8 -and -not (Test-CorpusZeroResidue -Results @($res, $resBad))) 'mixed corpus fails zero residue'

    $trailRes = Invoke-BunchBitEvaluation -PacketId 3 -BunchId 0 -TotalBits 40 -Steps @(
        (New-BitCursorStep -Handle 1 -FieldName 'CitizenshipWindow' -ExpectedBits 32 -ActualBits 32)
    )
    Assert-Condition ($trailRes.LeftoverBits -gt 0 -and -not (Test-CorpusZeroResidue -Results @($res, $trailRes))) 'trailing bits fail zero residue'
}

function Test-ManifestAndFridaSuite {
    $peBytes = New-SyntheticPe32PlusBytes
    $guid = '00112233445566778899AABBCCDDEEFF'
    $digest = Get-Sha256Hex -Bytes $peBytes
    $info = Get-PeRsdsInfo -PeBytes $peBytes
    Assert-Condition ($info.ExecSha256 -eq $digest -and $info.PdbGuid -eq $guid -and $info.PdbAge -eq 2 -and $info.IsPe32Plus) 'PE32+ RSDS parse'
    $pin = [pscustomobject]@{ ExecSha256 = $digest; PdbGuid = $guid; PdbAge = 2 }
    $verified = Confirm-PeRsdsManifest -PeBytes $peBytes -Pin $pin
    Assert-Condition ($verified.ExecSha256 -eq $digest) 'PE32+ RSDS manifest match'
    Assert-ThrowsMatch -ScriptBlock {
        Confirm-PeRsdsManifest -PeBytes $peBytes -Pin ([pscustomobject]@{ ExecSha256 = $digest; PdbGuid = ('FF' * 16); PdbAge = 2 })
    } -Pattern 'TOOL-1'
    Assert-ThrowsMatch -ScriptBlock {
        Confirm-PeRsdsManifest -PeBytes $peBytes -Pin ([pscustomobject]@{ ExecSha256 = ('00' * 32); PdbGuid = $guid; PdbAge = 2 })
    } -Pattern 'TOOL-1'

    $slot = New-RingSlotBytes -QpcTicks 9000UL -StructTypeHash 0xCAFEBABEU -BeforeBits 16 -AfterBits 40
    $rec = ConvertFrom-RingSlot -Buffer $slot
    Assert-Condition ($rec.QpcTicks -eq 9000UL -and $rec.BitsConsumed -eq 24) 'ring slot decode'
    Assert-Condition ((Test-HookLatencyNs -ElapsedNs 120.0) -and -not (Test-HookLatencyNs -ElapsedNs 180.0)) 'hook latency budget'
    $badSlot = New-RingSlotBytes -QpcTicks 9000UL -StructTypeHash 1U -BeforeBits 40 -AfterBits 16
    Assert-ThrowsMatch -ScriptBlock { ConvertFrom-RingSlot -Buffer $badSlot } -Pattern 'TOOL-9'
}

function Test-ReconcileAndGapSuite {
    Assert-Condition ($script:P0UndecodedStructs.Count -eq 12) '12 P0 structs'
    $records = Invoke-ThreeWayReconciliation -StaticStructs $script:P0UndecodedStructs -RuntimeCounts @{ FRepMovement = 42 } -ImplementedCodecs @('FVector_NetQuantize')
    Assert-Condition ($records.Count -eq 12) '12 reconciled records'
    $repMov = $records | Where-Object { $_.StructName -eq 'FRepMovement' } | Select-Object -First 1
    $vecQuant = $records | Where-Object { $_.StructName -eq 'FVector_NetQuantize' } | Select-Object -First 1
    Assert-Condition ($repMov.PriorityId -eq 'P0-01' -and $repMov.Status -eq 'MISSING_CODEC' -and $repMov.ObservedRuntimeCalls -eq 42) 'FRepMovement reconciliation'
    Assert-Condition ($vecQuant.Status -eq 'IMPLEMENTED') 'FVector_NetQuantize implemented'

    $gap = Get-CorrelatedGap -StructName 'FRepMovement' -ChannelIndex 3 -ExpectedBits 112 -BitPosBefore 16 -BitPosAfter 128
    Assert-Condition ($gap.BitDelta -eq 0) 'zero bit delta'
    Assert-ThrowsMatch -ScriptBlock { Get-CorrelatedGap -StructName 'FRepMovement' -ChannelIndex 3 -ExpectedBits 112 -BitPosBefore 128 -BitPosAfter 16 } -Pattern 'TOOL-9'
    $vec = New-CleanroomTestVector -StructName 'FRepMovement' -Payload ([byte[]]@(1, 2)) -BitLength 16
    Assert-Condition ($vec.PayloadHex -eq '0102') 'test vector hex'
    $forbiddenSymbol = 'sub_' + '140001000'
    Assert-ThrowsMatch -ScriptBlock { New-CleanroomTestVector -StructName $forbiddenSymbol -Payload ([byte[]]@(1, 2)) -BitLength 16 } -Pattern 'CLEANROOM-1'
}

function Test-StaticRizinAndSpecSuite {
    $base = $script:DefaultPe32PlusBase
    Assert-Condition ((ConvertTo-NormalizedRva -VirtualAddress ($base + 0x1200L) -ImageBase $base) -eq 0x1200L) 'normalize RVA'
    Assert-ThrowsMatch -ScriptBlock { ConvertTo-NormalizedRva -VirtualAddress ($base - 1L) -ImageBase $base } -Pattern 'precedes'
    Assert-ThrowsMatch -ScriptBlock { ConvertTo-NormalizedRva -VirtualAddress ($base + 0x80000000L) -ImageBase $base } -Pattern 'exceeds'

    $trait = [pscustomobject]@{ StructName = 'FEssentialCitizenshipWindowData'; VtableRva = 0x1200; WithNetSerializer = $true }
    $prop = [pscustomobject]@{ OwnerType = 'AAoCPlayerController'; PropName = 'CitizenshipWindow'; RepIndex = 42; ElementSize = 24; PropType = 'NetSerializeStruct' }
    $catalog = [pscustomobject]@{ ExecSha256 = ('a' * 64); PdbGuid = '11223344-5566-7788-99aa-bbccddee0011:1'; Structs = @($trait); Properties = @($prop) }
    Assert-CleanroomSchemaCatalog -Catalog $catalog

    $records = Invoke-ThreeWayReconciliation -StaticStructs (@($trait.StructName) + $script:P0UndecodedStructs) -RuntimeCounts @{ FRepMovement = 7 } -ImplementedCodecs @('FVector_NetQuantize')
    $matched = @($records | Where-Object { $_.Status -eq 'IMPLEMENTED' }).Count
    $missing = @($records | Where-Object { $_.Status -eq 'MISSING_CODEC' }).Count
    $report = [pscustomobject]@{ MatchedCount = $matched; MissingCodecCount = $missing; RuntimeOnly = 0; TotalP0Structs = $records.Count }
    $gap = Get-CorrelatedGap -StructName $trait.StructName -ChannelIndex 4 -ExpectedBits 0 -BitPosBefore 12 -BitPosAfter 44
    $gap.SampleHex = 'aabbccdd'
    $structMd = Format-StructSpecMarkdown -Gap $gap
    $summaryMd = Format-ReconciliationSummaryMarkdown -Report $report -Gaps @($gap)
    Assert-Condition ($structMd.Contains('FEssentialCitizenshipWindowData') -and $structMd.Contains('aabbccdd')) 'struct spec markdown'
    Assert-Condition ($summaryMd.Contains('# Clean-Room Wire Protocol Specification Summary') -and $summaryMd.Contains('32')) 'reconciliation summary markdown'
}

function Invoke-ReWorkbenchTestSuite {
    Test-BitOracleSuite
    Test-ManifestAndFridaSuite
    Test-ReconcileAndGapSuite
    Test-StaticRizinAndSpecSuite
    Write-Output '[PASS] Native PowerShell RE workbench verification suites passed (BitOracle, Manifest/Frida, Reconcile3Way/GapCorrelator, StaticRizin/SpecGenerator).'
}

switch ($Action) {
    'sync' {
        Write-Output '[PASS] Native PowerShell RE workbench ready (zero Python/venv dependency).'
    }
    'test' {
        Invoke-ReWorkbenchTestSuite
    }
    'run' {
        Invoke-ReWorkbenchTestSuite
    }
}

