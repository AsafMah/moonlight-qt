param(
    [Parameter(Mandatory=$true)][string]$WorkspaceDir,
    [Parameter(Mandatory=$true)][string]$InstallDir,
    [Parameter(Mandatory=$true)][string]$ZipPath,
    [Parameter(Mandatory=$true)][string]$ExePath,
    [Parameter(Mandatory=$true)][string]$ExpectedSha256
)

$ErrorActionPreference = 'Stop'
$extractDir = Join-Path $WorkspaceDir 'extract'
$backupDir = Join-Path $WorkspaceDir 'backup'
$workspaceFullPath = [System.IO.Path]::GetFullPath($WorkspaceDir).TrimEnd('\', '/')
$installFullPath = [System.IO.Path]::GetFullPath($InstallDir).TrimEnd('\', '/')
if ((Split-Path -Leaf $workspaceFullPath) -notmatch '^MoonlightPortableUpdate-\d+-\d+$' -or
    [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($ZipPath)) -ne $workspaceFullPath -or
    [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($ExePath)) -ne $installFullPath -or
    $installFullPath -eq $env:USERPROFILE -or
    $installFullPath -eq ([System.IO.Path]::GetPathRoot($InstallDir).TrimEnd('\', '/'))) {
    throw 'Unsafe portable update workspace or installation path.'
}
$backupStarted = $false
$copyStarted = $false
$committed = $false
$cleanupSafe = $false
$replacementNames = @()

function Get-RequiredFreeBytes {
    param([string]$ArchivePath)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $uncompressedBytes = ($archive.Entries | Measure-Object -Property Length -Sum).Sum
        if ($null -eq $uncompressedBytes) {
            $uncompressedBytes = 0
        }

        return [int64]$uncompressedBytes + 64MB
    }
    finally {
        $archive.Dispose()
    }
}

function Assert-FreeSpace {
    param(
        [string]$Path,
        [int64]$RequiredBytes
    )

    if ($RequiredBytes -le 0) {
        return
    }

    $drive = New-Object System.IO.DriveInfo(([System.IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $Path).Path)))
    if ($drive.AvailableFreeSpace -lt $RequiredBytes) {
        throw "Not enough free disk space for the portable update. Need about $([math]::Ceiling($RequiredBytes / 1MB)) MB free."
    }
}

function Wait-ForUnlockedFile {
    param([string]$Path)

    for ($i = 0; $i -lt 120; $i++) {
        try {
            $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
            if ($stream) {
                $stream.Dispose()
            }
            return
        }
        catch {
            Start-Sleep -Milliseconds 500
        }
    }

    throw 'Timed out waiting for Moonlight to exit.'
}

function Get-ReplacedItems {
    param([string]$Path)

    Get-ChildItem -LiteralPath $Path -Force | Where-Object {
        $itemFullPath = [System.IO.Path]::GetFullPath($_.FullName).TrimEnd('\', '/')

        $_.Name -in $replacementNames -and
        ![string]::Equals($itemFullPath, $workspaceFullPath, [System.StringComparison]::OrdinalIgnoreCase)
    }
}

try {
    if ($ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash -ne $ExpectedSha256) {
        throw 'The update package failed its integrity check.'
    }
    Wait-ForUnlockedFile -Path $ExePath

    if (!(Test-Path -LiteralPath $WorkspaceDir)) {
        New-Item -ItemType Directory -Path $WorkspaceDir | Out-Null
    }

    if (Test-Path -LiteralPath $extractDir) {
        Remove-Item -LiteralPath $extractDir -Recurse -Force
    }

    if (Test-Path -LiteralPath $backupDir) {
        Remove-Item -LiteralPath $backupDir -Recurse -Force
    }

    New-Item -ItemType Directory -Path $extractDir | Out-Null
    New-Item -ItemType Directory -Path $backupDir | Out-Null

    $requiredFreeBytes = Get-RequiredFreeBytes -ArchivePath $ZipPath
    Assert-FreeSpace -Path $InstallDir -RequiredBytes $requiredFreeBytes

    Expand-Archive -LiteralPath $ZipPath -DestinationPath $extractDir -Force

    $packageItems = @(Get-ChildItem -LiteralPath $extractDir -Force | Where-Object {
        $_.Name -notin @('Moonlight Game Streaming Project', 'Moonlight.ini', 'portable.dat',
                        'logs', 'dumps', 'cache', 'qmlcache', 'boxart')
    })
    if (!($packageItems | Where-Object { $_.Name -eq (Split-Path -Leaf $ExePath) -and !$_.PSIsContainer })) {
        throw 'The update package does not contain Moonlight.'
    }
    $replacementNames = @($packageItems | ForEach-Object { $_.Name })
    $itemsToReplace = @(Get-ReplacedItems -Path $InstallDir)
    foreach ($item in $itemsToReplace) {
        Move-Item -LiteralPath $item.FullName -Destination $backupDir -Force
        $backupStarted = $true
    }

    $copyStarted = $true
    $packageItems | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $InstallDir -Recurse -Force
    }

    $committed = $true
    try {
        Remove-Item -LiteralPath $backupDir -Recurse -Force
        $cleanupSafe = $true
    }
    catch {
        Write-Warning "Update committed; old backup is retained at $backupDir because cleanup failed: $_"
    }

    $updated = Start-Process -FilePath $ExePath -WorkingDirectory $InstallDir -PassThru
    "updated-process-pid=$($updated.Id)" | Out-File -LiteralPath (Join-Path $InstallDir 'Moonlight-update.log') -Append
}
catch {
    $failure = $_
    try {
        if (!$committed -and $backupStarted -and $backupDir -and (Test-Path -LiteralPath $backupDir)) {
            if ($copyStarted) {
                Get-ReplacedItems -Path $InstallDir | ForEach-Object {
                    Remove-Item -LiteralPath $_.FullName -Recurse -Force
                }
            }

            Get-ChildItem -LiteralPath $backupDir -Force | ForEach-Object {
                Move-Item -LiteralPath $_.FullName -Destination $InstallDir -Force
            }
        }
        if (!$committed) { $cleanupSafe = $true }
    }
    catch {
        $failure = [System.Management.Automation.ErrorRecord]::new(
            [System.Exception]::new("$($failure.Exception.Message)`nRollback failed: $_`nBackup retained at $backupDir"),
            'UpdateRollbackFailed', [System.Management.Automation.ErrorCategory]::WriteError, $backupDir)
    }

    $logPath = Join-Path $InstallDir 'Moonlight-update-error.log'
    $failure | Out-File -LiteralPath $logPath -Append -Encoding utf8
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show(
        "$($failure.Exception.Message)`nMoonlight will reopen. You can try the update again.",
        'Moonlight update failed') | Out-Null
    if (Test-Path -LiteralPath $ExePath) {
        Start-Process -FilePath $ExePath -WorkingDirectory $InstallDir | Out-Null
    }
}
finally {
    try {
        Set-Location -LiteralPath $InstallDir
    }
    catch {
        Write-Warning "Unable to leave updater workspace before cleanup: $_"
    }

    if (Test-Path -LiteralPath $extractDir) {
        Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    if ($cleanupSafe -and $backupDir -and (Test-Path -LiteralPath $backupDir)) {
        Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    $workspaceDir = [System.IO.Path]::GetDirectoryName($ZipPath)
    if ($cleanupSafe -and (Test-Path -LiteralPath $workspaceDir)) {
        Remove-Item -LiteralPath $workspaceDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
