param(
    [Parameter(Mandatory)][string]$ScriptPath,
    [Parameter(Mandatory)][string]$InstallDir,
    [Parameter(Mandatory)][string]$WorkspaceDir,
    [Parameter(Mandatory)][string]$ZipPath,
    [Parameter(Mandatory)][string]$Digest
)
$ErrorActionPreference = 'Stop'
function Start-Process {
    param($FilePath, $WorkingDirectory, [switch]$PassThru)
    'relaunch' | Set-Content -LiteralPath (Join-Path $InstallDir 'test-relaunch')
    return [pscustomobject]@{ Id = 12345 }
}
$parameters = @{
    WorkspaceDir = $WorkspaceDir
    InstallDir = $InstallDir
    ZipPath = $ZipPath
    ExePath = (Join-Path $InstallDir 'Moonlight.exe')
}
if ((Get-Command $ScriptPath).Parameters.ContainsKey('ExpectedSha256')) {
    $parameters.ExpectedSha256 = $Digest
}
. $ScriptPath @parameters
if (!(Test-Path -LiteralPath (Join-Path $InstallDir 'Moonlight.exe')) -or
    !(Test-Path -LiteralPath (Join-Path $InstallDir 'sentinel.txt'))) {
    throw 'A verified but invalid update archive must not erase the existing installation.'
}
'Portable rollback retained the original installation'
