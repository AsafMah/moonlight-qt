param(
    [Parameter(Mandatory)][string]$WorkspaceDir,
    [Parameter(Mandatory)][string]$InstallerPath,
    [Parameter(Mandatory)][string]$ExpectedSha256,
    [Parameter(Mandatory)][string]$ExePath,
    [Parameter(Mandatory)][int]$ParentPid
)

$ErrorActionPreference = 'Stop'
$workspaceFullPath = [System.IO.Path]::GetFullPath($WorkspaceDir).TrimEnd('\', '/')
if ((Split-Path -Leaf $workspaceFullPath) -notmatch '^MoonlightPortableUpdate-\d+-\d+$' -or
    [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($InstallerPath)) -ne $workspaceFullPath -or
    [System.IO.Path]::GetFullPath($ExePath).StartsWith($workspaceFullPath + '\',
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Unsafe installer update workspace.'
}
$ready = $false
$parentExited = $false
$restartRequired = $false
$installerStream = $null
Set-Location -LiteralPath ([System.IO.Path]::GetTempPath())
function Show-UpdateMessage {
    param([string]$Message, [string]$Title)
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show($Message, $Title) | Out-Null
}
try {
    $installerStream = [System.IO.File]::Open($InstallerPath,
        [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    $hashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $actualHash = ([System.BitConverter]::ToString(
            $hashAlgorithm.ComputeHash($installerStream))).Replace('-', '')
    }
    finally { $hashAlgorithm.Dispose() }
    if ($ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        $actualHash -ne $ExpectedSha256) {
        throw 'The update installer failed its integrity check.'
    }
    'ready' | Set-Content -LiteralPath (Join-Path $WorkspaceDir 'setup-ready') -Encoding ASCII
    $ready = $true
    $attempts = 0
    while (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue) {
        if (++$attempts -ge 600) { throw 'Moonlight did not exit in time; the installer was not started.' }
        Start-Sleep -Milliseconds 100
    }
    $parentExited = $true
    $installer = Start-Process -FilePath $InstallerPath -Verb RunAs -PassThru `
        -ArgumentList '/install', '/passive', '/norestart'
    $installer.WaitForExit()
    if ($installer.ExitCode -notin 0, 3010) {
        throw "The update installer exited with code $($installer.ExitCode)."
    }
    $restartRequired = $installer.ExitCode -eq 3010
    if ($restartRequired) {
        Show-UpdateMessage `
            'Windows must restart to finish the Moonlight update. Restart Windows before opening Moonlight.' `
            'Moonlight update requires restart'
    }
}
catch {
    $message = $_.Exception.Message
    $message | Set-Content -LiteralPath (Join-Path $WorkspaceDir 'setup-error.txt') -Encoding UTF8
    if (!$ready) {
        exit 1
    }
    Show-UpdateMessage $message 'Moonlight update failed'
}
finally {
    if ($installerStream) { $installerStream.Dispose() }
    if ($ready) {
        try {
            if ($parentExited -and !$restartRequired) {
                Start-Process -FilePath $ExePath -WorkingDirectory (Split-Path -Parent $ExePath) | Out-Null
            }
        }
        finally {
            try {
                Remove-Item -LiteralPath $workspaceFullPath -Recurse -Force
            }
            catch {
                Write-Warning "Unable to clean installer workspace $workspaceFullPath : $_"
            }
        }
    }
}
