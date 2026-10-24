# -PyroWave Auto|Require|Skip controls the optional PyroWave runtime build
# (see scripts\build-pyrowave.ps1). Auto continues without it on failure.
param(
    [ValidateSet('Auto', 'Require', 'Skip')]
    [string]$PyroWave = 'Auto'
)

$ErrorActionPreference = 'Stop'

$Organization = "moonlight-stream"
$PrebuiltRepo = "moonlight-qt-deps"
$TargetDir = Join-Path $PSScriptRoot "libs\windows"
$Assets = @("windows-x64.zip", "windows-ARM64.zip")
$Tag = "v19"

if (Test-Path $TargetDir) {
    Write-Host "Cleaning target directory..." -ForegroundColor Cyan
    Remove-Item -Path "$TargetDir\*" -Recurse -Force
} else {
    New-Item -ItemType Directory -Path $TargetDir | Out-Null
}

foreach ($AssetName in $Assets) {
    $Url = "https://github.com/$Organization/$PrebuiltRepo/releases/download/$Tag/$AssetName"
    $ArchivePath = Join-Path $env:TEMP $AssetName

    Write-Host "Downloading $AssetName..." -ForegroundColor Cyan
    curl.exe -s -L -f -o "$ArchivePath" "$Url"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    Write-Host "Extracting $AssetName..." -ForegroundColor Cyan
    Expand-Archive -Path $ArchivePath -DestinationPath $TargetDir -Force
    Remove-Item $ArchivePath
}

# Build the PyroWave runtime next to the prebuilt DLLs so build-arch.bat
# deploys it (and its PDB) with them.
foreach ($Arch in @('x64', 'arm64')) {
    # Granite's math library has no MSVC ARM64 SIMD path yet ("Implement me."
    # in simd.hpp), so arm64 builds ship without PyroWave, which hides it.
    $Mode = if ($Arch -eq 'arm64') { 'Skip' } else { $PyroWave }
    & (Join-Path $PSScriptRoot 'scripts\build-pyrowave.ps1') -Arch $Arch -Mode $Mode `
        -OutDir (Join-Path $TargetDir "lib\$Arch")
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

Write-Host "Dependencies successfully deployed" -ForegroundColor Green