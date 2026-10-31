param([string]$ScriptPath, [string]$WorkspaceDir, [string]$InstallerPath,
      [string]$ExePath, [string]$Digest, [string]$Mode, [string]$TracePath)
$ErrorActionPreference = 'Stop'
function Start-Process {
    param($FilePath, $Verb, [switch]$PassThru, $ArgumentList, $WorkingDirectory)
    "$Verb|$FilePath|$ArgumentList" | Add-Content -LiteralPath $TracePath
    if ($Verb -eq 'RunAs') {
        if ($Mode -eq 'cancel') { throw 'The operation was canceled by the user.' }
        $result = [PSCustomObject]@{ ExitCode = $(if ($Mode -eq 'installer-failure') { 1603 } else { 0 }) }
        $result | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {}
        return $result
    }
}
function Get-Process { param($Id, $ErrorAction) }
. $ScriptPath -WorkspaceDir $WorkspaceDir -InstallerPath $InstallerPath `
    -ExpectedSha256 $Digest -ExePath $ExePath -ParentPid 1
if ($LASTEXITCODE) { exit $LASTEXITCODE }
