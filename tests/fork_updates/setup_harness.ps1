param([string]$ScriptPath, [string]$WorkspaceDir, [string]$InstallerPath,
      [string]$ExePath, [string]$Digest, [string]$Mode, [string]$TracePath)
$ErrorActionPreference = 'Stop'
function Start-Process {
    param($FilePath, $Verb, [switch]$PassThru, $ArgumentList, $WorkingDirectory)
    "$Verb|$FilePath|$ArgumentList" | Add-Content -LiteralPath $TracePath
    if ($Verb -eq 'RunAs') {
        if ((Get-Location).Path.TrimEnd('\') -ne [IO.Path]::GetTempPath().TrimEnd('\')) {
            throw 'Installer helper must not hold its workspace as the current directory.'
        }
        if (!(Test-Path -LiteralPath $TracePath) -or
            !(Select-String -LiteralPath $TracePath -Pattern '^parent-exited$' -Quiet)) {
            throw 'Installer must not start before Moonlight exits.'
        }
        $blocked = $false
        try { [IO.File]::WriteAllText($FilePath, 'tampered') }
        catch [IO.IOException] { $blocked = $true }
        if (!$blocked) {
            'Installer must remain read-locked at the execution boundary.' | Add-Content -LiteralPath $TracePath
            throw 'Installer must remain read-locked at the execution boundary.'
        }
        $blocked = $false
        try { [IO.File]::Move($FilePath, $FilePath + '.replaced') }
        catch [IO.IOException] { $blocked = $true }
        if (!$blocked) { throw 'Installer replacement must be blocked at the execution boundary.' }
        if ($Mode -eq 'cancel') { throw 'The operation was canceled by the user.' }
        $result = [PSCustomObject]@{ ExitCode = $(if ($Mode -eq 'installer-failure') { 1603 } elseif ($Mode -eq 'reboot') { 3010 } else { 0 }) }
        $result | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {}
        return $result
    }
}
function Get-Process {
    param($Id, $ErrorAction)
    'parent-exited' | Add-Content -LiteralPath $TracePath
    if ($Mode -eq 'parent-timeout') { return [PSCustomObject]@{ Id = $Id } }
}
function Start-Sleep { param($Milliseconds) }
. $ScriptPath -WorkspaceDir $WorkspaceDir -InstallerPath $InstallerPath `
    -ExpectedSha256 $Digest -ExePath $ExePath -ParentPid 1
if ($LASTEXITCODE) { exit $LASTEXITCODE }
