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
$installed = $false
try {
    if ($ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        (Get-FileHash -LiteralPath $InstallerPath -Algorithm SHA256).Hash -ne $ExpectedSha256) {
        throw 'The update installer failed its integrity check.'
    }
    $installer = Start-Process -FilePath $InstallerPath -Verb RunAs -PassThru `
        -ArgumentList '/install', '/passive', '/norestart'
    'ready' | Set-Content -LiteralPath (Join-Path $WorkspaceDir 'setup-ready') -Encoding ASCII
    $ready = $true
    while (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue) {
        Start-Sleep -Milliseconds 100
    }
    $installer.WaitForExit()
    if ($installer.ExitCode -notin 0, 3010) {
        throw "The update installer exited with code $($installer.ExitCode)."
    }
    $installed = $true
}
catch {
    $message = $_.Exception.Message
    $message | Set-Content -LiteralPath (Join-Path $WorkspaceDir 'setup-error.txt') -Encoding UTF8
    if (!$ready) {
        exit 1
    }
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show(
        "$message`nMoonlight will reopen. You can try the update again.",
        'Moonlight update failed') | Out-Null
}
finally {
    if ($ready) {
        Start-Process -FilePath $ExePath -WorkingDirectory (Split-Path -Parent $ExePath) | Out-Null
        if ($installed) {
            Remove-Item -LiteralPath $workspaceFullPath -Recurse -Force
        }
    }
}
