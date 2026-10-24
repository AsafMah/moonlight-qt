# Builds the PyroWave runtime (libpyrowave-shared-101.dll) for Windows and
# copies the DLL and its PDB to the given directory. setup-deps.ps1 calls this
# for x64 and arm64 so build-arch.bat deploys the DLL with the other
# prebuilt libraries.
#
# -Mode Auto    (default) warn and continue without the runtime; PyroWave is hidden
# -Mode Require fail, for CI and release builds
# -Mode Skip    do nothing
#
# third-party/pyrowave is marked "update = none" in .gitmodules so recursive
# checkouts skip its large dependency tree. This script fetches only what the
# runtime build needs: pyrowave, Granite, volk and the Vulkan headers.
#
# Keep this compatible with Windows PowerShell 5.1 (build-arch.bat uses it).

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'arm64')]
    [string]$Arch,

    [Parameter(Mandatory = $true)]
    [string]$OutDir,

    [ValidateSet('Auto', 'Require', 'Skip')]
    [string]$Mode = 'Auto'
)

$ErrorActionPreference = 'Stop'

$SourceRoot = Split-Path -Parent $PSScriptRoot
$PyrowaveDir = Join-Path $SourceRoot 'third-party\pyrowave'
$GraniteDir = Join-Path $PyrowaveDir 'third_party\Granite'
$LibraryName = 'libpyrowave-shared-101'

function Stop-PyrowaveBuild([string]$Reason) {
    if ($Mode -eq 'Require') {
        Write-Host "PyroWave runtime ($Arch): $Reason" -ForegroundColor Red
        exit 1
    }
    Write-Warning "PyroWave runtime ($Arch): $Reason. Continuing without it; PyroWave will be hidden."
    exit 0
}

# Native tools write progress to stderr. Windows PowerShell 5.1 can turn that
# into a terminating error under 'Stop', so run them under 'Continue' and rely
# on the exit code instead.
function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # Send output to the console so it is not mistaken for the return value.
        & $Exe @Arguments | Out-Host
        return ($LASTEXITCODE -eq 0)
    }
    finally {
        $ErrorActionPreference = $previous
    }
}

# Shallow first; fall back to a full fetch for servers that refuse a shallow
# fetch of an unadvertised commit.
function Update-Submodule([string]$Repo, [string[]]$Paths) {
    $common = @('-c', 'core.longpaths=true', '-C', $Repo, 'submodule', 'update', '--init', '--checkout')
    if (Invoke-Native 'git' ($common + @('--depth', '1') + $Paths)) {
        return $true
    }
    return (Invoke-Native 'git' ($common + $Paths))
}

function Find-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    # Visual Studio ships CMake with its C++ workload.
    $vswhere = Join-Path $PSScriptRoot 'vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' |
            Select-Object -First 1
        if ($found -and (Test-Path $found)) {
            return $found
        }
    }
    return $null
}

if ($Mode -eq 'Skip') {
    Write-Host "PyroWave runtime build skipped ($Arch)"
    exit 0
}

if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
    Stop-PyrowaveBuild 'git was not found'
}

$CMake = Find-CMake
if (-not $CMake) {
    Stop-PyrowaveBuild 'cmake was not found'
}

# PyroWave names the DLL with DLL_NAME_WITH_SOVERSION, which needs CMake 3.27.
$versionLine = (& $CMake --version | Select-Object -First 1)
if ($versionLine -match '(\d+)\.(\d+)') {
    $cmakeVersion = [version]"$($Matches[1]).$($Matches[2])"
    if ($cmakeVersion -lt [version]'3.27') {
        Stop-PyrowaveBuild "CMake 3.27 or newer is required (found $cmakeVersion)"
    }
}

Write-Host "Fetching PyroWave sources" -ForegroundColor Cyan
if (-not (Update-Submodule $SourceRoot @('third-party/pyrowave'))) {
    Stop-PyrowaveBuild 'unable to fetch third-party/pyrowave'
}
if (-not (Update-Submodule $PyrowaveDir @('third_party/Granite'))) {
    Stop-PyrowaveBuild 'unable to fetch Granite'
}
if (-not (Update-Submodule $GraniteDir @('third_party/volk', 'third_party/khronos/vulkan-headers'))) {
    Stop-PyrowaveBuild 'unable to fetch volk and the Vulkan headers'
}

$BuildDir = Join-Path $SourceRoot "build\pyrowave-runtime-$Arch"
if (Test-Path $BuildDir) {
    Remove-Item -Path $BuildDir -Recurse -Force
}

$Platform = if ($Arch -eq 'arm64') { 'ARM64' } else { 'x64' }
$RuntimeSource = Join-Path $SourceRoot 'third-party\pyrowave-runtime'

Write-Host "Building PyroWave runtime ($Arch) in $BuildDir" -ForegroundColor Cyan
if (-not (Invoke-Native $CMake @('-S', $RuntimeSource, '-B', $BuildDir, '-A', $Platform, '-DCMAKE_BUILD_TYPE=Release'))) {
    Stop-PyrowaveBuild 'CMake configuration failed'
}
if (-not (Invoke-Native $CMake @('--build', $BuildDir, '--config', 'Release', '--target', 'pyrowave-shared', '--parallel'))) {
    Stop-PyrowaveBuild 'the CMake build failed'
}

$BuiltDll = Join-Path $BuildDir "out\$LibraryName.dll"
if (-not (Test-Path $BuiltDll)) {
    Stop-PyrowaveBuild "the build did not produce $LibraryName.dll"
}

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}
Copy-Item -Path $BuiltDll -Destination $OutDir -Force

$BuiltPdb = Join-Path $BuildDir "out\$LibraryName.pdb"
if (Test-Path $BuiltPdb) {
    Copy-Item -Path $BuiltPdb -Destination $OutDir -Force
}

Write-Host "PyroWave runtime ($Arch): $(Join-Path $OutDir "$LibraryName.dll")" -ForegroundColor Green
