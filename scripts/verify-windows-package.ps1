param(
    [Parameter(Mandatory=$true)]
    [string]$ExePath,

    [Parameter(Mandatory=$true)]
    [string]$MsiPath,

    [string]$BundlePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expected = & python "$PSScriptRoot\derive-version.py" --field numeric
if ($LASTEXITCODE -ne 0) { throw 'Failed to derive numeric version' }
$artifact = & python "$PSScriptRoot\derive-version.py" --field artifact
if ($LASTEXITCODE -ne 0) { throw 'Failed to derive artifact version' }

# Check independently of derive-version's numeric path, which once dropped CI's count.
if ($artifact -match '[+-](\d+)\.g[0-9a-fA-F]+(?:\.dirty)?$') {
    $revision = [Math]::Min([long]$Matches[1], 65535)
    if (([version]$expected).Revision -ne $revision) {
        throw "Development build $artifact lost its revision in numeric version $expected"
    }
}

$exe = (Get-Item -LiteralPath $ExePath).VersionInfo
if ($exe.FileVersion -ne $expected -or $exe.ProductVersion -ne $expected) {
    throw "EXE versions ($($exe.FileVersion), $($exe.ProductVersion)) != $expected"
}
if ($BundlePath) {
    $bundle = (Get-Item -LiteralPath $BundlePath).VersionInfo
    if ($bundle.FileVersion -ne $expected) {
        throw "Setup FileVersion $($bundle.FileVersion) != $expected"
    }
}

$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.OpenDatabase((Resolve-Path -LiteralPath $MsiPath).Path, 0)
function Read-MsiValue([string]$Query) {
    $view = $database.OpenView($Query)
    try {
        [void]$view.Execute()
        $record = $view.Fetch()
        if ($null -eq $record) { throw "Missing MSI row: $Query" }
        try { return $record.StringData(1) }
        finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) }
    }
    finally {
        [void]$view.Close()
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
    }
}

try {
    $productVersion = Read-MsiValue 'SELECT `Value` FROM `Property` WHERE `Property` = ''ProductVersion'''
    $fileVersion = Read-MsiValue 'SELECT `Version` FROM `File` WHERE `File` = ''MoonlightExe'''
    if ($productVersion -ne $expected -or $fileVersion -ne $expected) {
        throw "MSI versions (ProductVersion=$productVersion, MoonlightExe=$fileVersion) != $expected"
    }
    $attributes = [int](Read-MsiValue 'SELECT `Attributes` FROM `Upgrade` WHERE `ActionProperty` = ''WIX_UPGRADE_DETECTED''')
    $maximum = Read-MsiValue 'SELECT `VersionMax` FROM `Upgrade` WHERE `ActionProperty` = ''WIX_UPGRADE_DETECTED'''
    if (($attributes -band 512) -eq 0 -or ($attributes -band 2) -ne 0 -or $maximum -ne $expected) {
        throw "MSI does not remove equal three-field versions (attributes=$attributes, max=$maximum)"
    }
    $initialize = [int](Read-MsiValue 'SELECT `Sequence` FROM `InstallExecuteSequence` WHERE `Action` = ''InstallInitialize''')
    $remove = [int](Read-MsiValue 'SELECT `Sequence` FROM `InstallExecuteSequence` WHERE `Action` = ''RemoveExistingProducts''')
    $files = [int](Read-MsiValue 'SELECT `Sequence` FROM `InstallExecuteSequence` WHERE `Action` = ''InstallFiles''')
    if ($remove -le $initialize -or $remove -ge $files) {
        throw 'MSI must remove the previous product inside the transaction before installing files'
    }
    Write-Host "Verified Windows package: artifact=$artifact EXE=$($exe.FileVersion) MSI=$productVersion; same-version upgrade enabled"
}
finally {
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
}
