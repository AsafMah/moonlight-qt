param(
    [Parameter(Mandatory=$true)]
    [string]$BundlePath,

    [Parameter(Mandatory=$true)]
    [string]$ExePath,

    [Parameter(Mandatory=$true)]
    [string]$WorkDirectory,

    [switch]$Install
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$BundlePath = (Resolve-Path -LiteralPath $BundlePath).Path
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$currentVersion = [version](Get-Item -LiteralPath $ExePath).VersionInfo.FileVersion
if ($Install -and ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted')) {
    throw 'Install/uninstall testing is restricted to disposable GitHub-hosted runners'
}
if ($currentVersion.Revision -lt 2) {
    throw 'Upgrade smoke requires a development build with a predecessor revision'
}
if (Test-Path -LiteralPath $WorkDirectory) {
    throw "Use a fresh work directory: $WorkDirectory"
}
New-Item -ItemType Directory -Path $WorkDirectory | Out-Null
$installer = New-Object -ComObject WindowsInstaller.Installer
$upgradeCode = '{5C09F94E-F809-4C6A-9B7B-597C99F041FE}'

function Get-RelatedProducts {
    return ,@($installer.RelatedProducts($upgradeCode))
}

function Invoke-Setup([string]$Setup, [string]$Operation, [string]$Log) {
    $process = Start-Process -FilePath $Setup -ArgumentList "$Operation /quiet /norestart /log `"$Log`"" -Wait -PassThru
    if ($process.ExitCode -notin @(0, 3010)) {
        throw "Setup failed ($($process.ExitCode)): $Setup; inspect $Log"
    }
}

function Build-Fixture([int]$Revision) {
    $version = '{0}.{1}.{2}.{3}' -f $currentVersion.Major, $currentVersion.Minor, $currentVersion.Build, $Revision
    $fixture = Join-Path $WorkDirectory "from-$Revision"
    $build = Join-Path $fixture 'build-x64-release'
    $deploy = Join-Path $fixture 'deploy'
    $app = Join-Path $build 'app\release'
    New-Item -ItemType Directory -Path $app, $deploy | Out-Null
    $source = @"
using System.Reflection;
[assembly: AssemblyVersion("$version")]
[assembly: AssemblyFileVersion("$version")]
class Fixture { static void Main() {} }
"@
    $source | Set-Content -LiteralPath "$fixture\fixture.cs" -Encoding ASCII
    'Legacy payload that must disappear during upgrade' | Set-Content "$deploy\legacy-upgrade-fixture.txt"
    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /platform:x64 "/out:$app\Moonlight.exe" "$fixture\fixture.cs" > "$fixture\compile.log" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile legacy fixture; inspect $fixture\compile.log" }
    $env:BUILD_FOLDER = $build
    $env:DEPLOY_FOLDER = $deploy
    & dotnet build "$root\wix\Moonlight\Moonlight.wixproj" --nologo -v:q -p:Configuration=release -p:Platform=x64 "-p:MSBuildProjectExtensionsPath=$build\" > "$fixture\msi-build.log" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Failed to build predecessor MSI; inspect $fixture\msi-build.log" }

    # Both packages need distinct ProductCodes to bind the production bundle.
    # The inert payload is never run; only x64 is installed on the hosted runner.
    $env:BUILD_FOLDER = "$fixture\build-arm64-release"
    New-Item -ItemType Directory -Path "$env:BUILD_FOLDER\app\release" | Out-Null
    Copy-Item "$app\Moonlight.exe" "$env:BUILD_FOLDER\app\release\Moonlight.exe"
    & dotnet build "$root\wix\Moonlight\Moonlight.wixproj" --nologo -v:q -p:Configuration=release -p:Platform=arm64 "-p:MSBuildProjectExtensionsPath=$env:BUILD_FOLDER\" > "$fixture\arm64-msi-build.log" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Failed to build predecessor arm64 MSI; inspect $fixture\arm64-msi-build.log" }
    $env:BUILD_ROOT = $fixture
    $env:BUILD_CONFIG = 'release'
    $env:BUILD_FOLDER = "$fixture\bundle"
    $env:INSTALLER_FOLDER = "$fixture\setup"
    & dotnet build "$root\wix\MoonlightSetup\MoonlightSetup.wixproj" --nologo -v:q -p:Configuration=release -p:Platform=x86 "-p:MSBuildProjectExtensionsPath=$fixture\bundle\" > "$fixture\bundle-build.log" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Failed to build predecessor bundle; inspect $fixture\bundle-build.log" }
    $setup = "$fixture\setup\MoonlightSetup.exe"
    if ((Get-Item -LiteralPath $setup).VersionInfo.FileVersion -ne $version) {
        throw "Fixture bundle version does not match $version"
    }
    return $setup
}

$saved = @{}
foreach ($name in @('BUILD_ROOT', 'BUILD_CONFIG', 'BUILD_FOLDER', 'DEPLOY_FOLDER', 'INSTALLER_FOLDER', 'VERSION', 'SIGN')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$env:VERSION = $null
$env:SIGN = $null
try {
    if ($Install -and (Get-RelatedProducts).Count -ne 0) {
        throw 'Refusing to overwrite an existing Moonlight installation'
    }
    foreach ($revision in @(0, ($currentVersion.Revision - 1))) {
        $fixtureSetup = Build-Fixture $revision
        Write-Host "Built predecessor bundle: $fixtureSetup"
        if (-not $Install) { continue }
        $installedExe = Join-Path $env:ProgramW6432 'Moonlight Game Streaming\Moonlight.exe'
        $settingsKey = 'HKCU:\Software\Moonlight Game Streaming Project'
        $sentinel = [guid]::NewGuid().ToString()
        try {
            Invoke-Setup $fixtureSetup '/install' "$WorkDirectory\install-$revision.log"
            $oldProducts = Get-RelatedProducts
            if ($oldProducts.Count -ne 1 -or ([version](Get-Item $installedExe).VersionInfo.FileVersion).Revision -ne $revision) {
                throw 'Predecessor installation did not establish the requested baseline'
            }
            New-ItemProperty -Path $settingsKey -Name UpgradeTestSentinel -Value $sentinel -Force | Out-Null
            Invoke-Setup $BundlePath '/install' "$WorkDirectory\upgrade-$revision.log"
            if ((Get-FileHash $installedExe).Hash -ne (Get-FileHash $ExePath).Hash) {
                throw "Upgrade from revision $revision did not replace Moonlight.exe with the published payload"
            }
            $newProducts = Get-RelatedProducts
            if ($newProducts.Count -ne 1 -or $newProducts[0] -eq $oldProducts[0]) {
                throw 'Upgrade left the old MSI installed or registered multiple products'
            }
            if (Test-Path (Join-Path (Split-Path $installedExe) 'legacy-upgrade-fixture.txt')) {
                throw 'Upgrade left obsolete predecessor payload behind'
            }
            if ((Get-ItemPropertyValue $settingsKey UpgradeTestSentinel) -ne $sentinel) {
                throw 'Upgrade deleted user settings'
            }
            Write-Host "PASS: revision $revision -> $currentVersion via setup; payload replaced, old MSI removed, settings preserved"
        }
        finally {
            Invoke-Setup $BundlePath '/uninstall' "$WorkDirectory\uninstall-current-$revision.log"
            Invoke-Setup $fixtureSetup '/uninstall' "$WorkDirectory\uninstall-fixture-$revision.log"
            if ((Get-RelatedProducts).Count -ne 0) { throw 'Upgrade smoke cleanup left a registered product' }
        }
    }
}
finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
}
