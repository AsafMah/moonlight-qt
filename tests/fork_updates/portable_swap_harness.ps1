param([string]$ScriptPath, [string]$WorkspaceDir, [string]$InstallDir,
      [string]$ZipPath, [string]$Digest, [string]$Mode, [string]$TracePath)
$ErrorActionPreference = 'Stop'
$script:backupLock = $null
function Move-Item {
    param($LiteralPath, $Destination, [switch]$Force, $ErrorAction)
    if ($Mode -eq 'partial-backup' -and
        $Destination -eq (Join-Path $WorkspaceDir 'backup') -and
        (Split-Path -Leaf $LiteralPath) -eq 'platforms') {
        throw 'Simulated failure partway through backing up replaced package names.'
    }
    Microsoft.PowerShell.Management\Move-Item @PSBoundParameters
}
function Start-Process {
    param($FilePath, $WorkingDirectory, [switch]$PassThru)
    'relaunch' | Add-Content -LiteralPath $TracePath
    return [PSCustomObject]@{ Id = 1234 }
}
function Remove-Item {
    param($LiteralPath, [switch]$Recurse, [switch]$Force, $ErrorAction)
    if ($Mode -eq 'locked-backup' -and (Split-Path -Leaf $LiteralPath) -eq 'backup') {
        $old = Join-Path $LiteralPath 'Moonlight.exe'
        if ((Test-Path -LiteralPath $old) -and !$script:backupLock) {
            $script:backupLock = [IO.File]::Open($old, 'Open', 'Read', 'Read')
        }
    }
    Microsoft.PowerShell.Management\Remove-Item @PSBoundParameters
}
$source = Get-Content -LiteralPath $ScriptPath -Raw
$pattern = '    Add-Type -AssemblyName System.Windows.Forms\r?\n    \[System.Windows.Forms.MessageBox\]::Show\([\s\S]+?\) \| Out-Null'
if (![regex]::IsMatch($source, $pattern)) { throw 'Harness must intercept the update failure dialog.' }
$source = [regex]::Replace($source, $pattern, "    'failure-dialog' | Add-Content -LiteralPath `$TracePath")
try {
    . ([scriptblock]::Create($source)) -WorkspaceDir $WorkspaceDir -InstallDir $InstallDir `
        -ZipPath $ZipPath -ExePath (Join-Path $InstallDir 'Moonlight.exe') -ExpectedSha256 $Digest
}
finally {
    if ($script:backupLock) { $script:backupLock.Dispose() }
}
if ([IO.File]::ReadAllText((Join-Path $InstallDir 'Moonlight.exe')) -ne 'new build') {
    if ($Mode -ne 'partial-backup' -or
        [IO.File]::ReadAllText((Join-Path $InstallDir 'Moonlight.exe')) -ne 'old build' -or
        [IO.File]::ReadAllText((Join-Path $InstallDir 'platforms\old-platform.dll')) -ne 'old dependency') {
        throw 'Failed partial backup must restore moved files and keep untouched originals.'
    }
    if (!(Select-String -LiteralPath $TracePath -Pattern '^failure-dialog$' -Quiet)) {
        throw 'A failed portable update must show a visible error.'
    }
    return
}
foreach ($path in @('sentinel.txt','logs\old.log','dumps\old.dmp','cache\old.cache')) {
    if (!(Test-Path -LiteralPath (Join-Path $InstallDir $path))) {
        throw "Package-only replacement must preserve unrelated file $path"
    }
}
if (!(Test-Path -LiteralPath (Join-Path $InstallDir 'platforms\qwindows.dll')) -or
    (Test-Path -LiteralPath (Join-Path $InstallDir 'platforms\old-platform.dll'))) {
    throw 'An existing package-owned directory must be replaced with the new package contents.'
}
