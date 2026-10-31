param(
    [string]$QtBin = 'C:\tmp\qt\6.12.0\msvc2022_64\bin',
    [string]$Jom = 'C:\tmp\qt\jom\jom.exe',
    [string]$VcVars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$Runner = 'E:\Code\moonlight-tools\run-test-exe.ps1',
    [string]$Artifacts,
    [string[]]$Dpr = @('1', '1.5', '2')
)
$ErrorActionPreference = 'Stop'
$oldDpr = $env:QT_SCALE_FACTOR
$oldArtifacts = $env:MENU_PHASE1_ARTIFACTS
try {
    & $env:ComSpec /c "call `"$VcVars`" >nul && cd /d `"$PSScriptRoot`" && `"$QtBin\qmake.exe`" menu_phase1.pro && `"$Jom`" -j 4"
    if ($LASTEXITCODE -ne 0) { throw "Panel gate build failed: $LASTEXITCODE" }
    $env:MENU_PHASE1_ARTIFACTS = $Artifacts
    foreach ($scale in $Dpr) {
        $env:QT_SCALE_FACTOR = $scale
        & pwsh -File $Runner -Exe "$PSScriptRoot\release\menu_phase1.exe" -Platform offscreen -QtBin $QtBin
        if ($LASTEXITCODE -ne 0) { throw "Panel gates failed at DPR $scale`: $LASTEXITCODE" }
    }
} finally {
    $env:QT_SCALE_FACTOR = $oldDpr
    $env:MENU_PHASE1_ARTIFACTS = $oldArtifacts
}
