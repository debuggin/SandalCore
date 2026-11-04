@{
    RootModule        = 'Stamps.psm1'
    ModuleVersion     = '0.1.0'
    GUID              = '8c4e17b0-6d2f-4a91-9e3b-5f7012c4a8d9'
    Author            = 'SandalCore'
    Description       = 'SandalCore 11-stage SHA-256 stamp DAG, heavy operation mutex, and CSPRNG environment management.'
    PowerShellVersion = '7.2'
    NestedModules     = @('Env.psm1')
    FunctionsToExport = @(
        'Get-SandalStageOrder', 'Get-SandalStageGraph', 'Compute-SandalStageHash',
        'Read-SandalStageStamp', 'Test-SandalStageUpToDate', 'Write-SandalStageStamp',
        'Invoke-WithHeavyOperationLock', 'New-SandalCsprngSecret', 'Get-SandalEnvMap',
        'Initialize-SandalEnv', 'Set-SandalSecretAcl', 'Sync-SandalDistStage',
        'Sync-SandalDeployStage', 'Ensure-SandalGameData'
    )
    CmdletsToExport   = @()
    VariablesToExport = @()
    AliasesToExport   = @()
}
