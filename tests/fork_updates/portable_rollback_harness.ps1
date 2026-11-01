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
$source = Get-Content -LiteralPath $ScriptPath -Raw
$pattern = '    Add-Type -AssemblyName System.Windows.Forms\r?\n    \[System.Windows.Forms.MessageBox\]::Show\([\s\S]+?\) \| Out-Null'
$source = [regex]::Replace($source, $pattern, "    'failure-dialog' | Set-Content -LiteralPath (Join-Path `$InstallDir 'test-dialog')")
. ([scriptblock]::Create($source)) @parameters
if (!(Test-Path -LiteralPath (Join-Path $InstallDir 'Moonlight.exe')) -or
    !(Test-Path -LiteralPath (Join-Path $InstallDir 'sentinel.txt'))) {
    throw 'A verified but invalid update archive must not erase the existing installation.'
}
'Portable rollback retained the original installation'
if (!(Test-Path -LiteralPath (Join-Path $InstallDir 'test-dialog'))) {
    throw 'A failed portable update must show a visible error.'
}
