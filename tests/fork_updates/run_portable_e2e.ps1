param([Parameter(Mandatory)][string]$Manifest, [Parameter(Mandatory)][string]$OlderExe,
      [Parameter(Mandatory)][string]$QtBin)
$ErrorActionPreference = 'Stop'
$fixture = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
$exe = $fixture.executable
if ($exe -notlike "$($fixture.fixture)\portable\*" -or
    (Get-FileHash -LiteralPath $OlderExe).Hash -eq $fixture.targetSha256) {
    throw 'The private source must be an actual older fixture build.'
}
Copy-Item -LiteralPath $OlderExe -Destination $exe
$install = Split-Path -Parent $exe
Get-ChildItem -LiteralPath "$PSScriptRoot\..\..\libs\windows\lib\x64" -Filter '*.dll' -File |
    Copy-Item -Destination $install
$settingsDirectory = Join-Path $install 'Moonlight Game Streaming Project'
New-Item -ItemType Directory -Path $settingsDirectory -Force | Out-Null
$settingsPath = Join-Path $settingsDirectory 'Moonlight.ini'
"`n[General]`nupdateFixtureSentinel=preserve-me" | Add-Content -LiteralPath $settingsPath
$env:QT_QUICK_CONTROLS_STYLE = 'FluentWinUI3'
$env:QML_XHR_ALLOW_FILE_READ = '1'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = [System.IO.Path]::GetFullPath("$QtBin\..\plugins\platforms")
if (!(Test-Path -LiteralPath "$env:QT_QPA_PLATFORM_PLUGIN_PATH\qoffscreen.dll")) {
    throw 'The matching offscreen Qt plugin is required for this private validation.'
}
$env:QML_DISABLE_DISK_CACHE = '1'
$started = [DateTime]::UtcNow
$applicationOutput = & "$PSScriptRoot\..\..\scripts\run-built-test.ps1" -Exe $exe -QtBin $QtBin -Platform offscreen
if ($LASTEXITCODE) { throw "The older app exited with $LASTEXITCODE" }
$deadline = [DateTime]::UtcNow.AddSeconds(90)
$relaunchedPid = $null
while ([DateTime]::UtcNow -lt $deadline) {
    $log = Join-Path $install 'Moonlight-update.log'
    if ((Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).LastWriteTimeUtc -ge $started) {
        $matches = [regex]::Matches((Get-Content -LiteralPath $log -Raw), 'updated-process-pid=(\d+)')
        if ($matches.Count) { $relaunchedPid = [int]$matches[-1].Groups[1].Value; break }
    }
    Start-Sleep -Milliseconds 200
}
if (!$relaunchedPid) { throw 'No verified replacement-app relaunch was observed.' }
$process = Get-Process -Id $relaunchedPid -ErrorAction Stop
try {
    if ($process.Path -ne $exe) { throw 'The reported process is not the isolated replacement.' }
    if ((Get-FileHash -LiteralPath $exe).Hash -ne $fixture.targetSha256) {
        throw 'The replacement is not the published r43 executable.'
    }
    $config = Get-Content -LiteralPath $settingsPath -Raw
    if (!$config.Contains('updateFixtureSentinel=preserve-me')) {
        throw 'The portable configuration sentinel was lost.'
    }
    $logs = @($applicationOutput) + @(Get-ChildItem -LiteralPath $install -Filter 'Moonlight-*.log' -Recurse |
        ForEach-Object { Get-Content -LiteralPath $_.FullName | Where-Object { $_ -match 'UPDATE_E2E_(OFFER|CONFIRMED)' } }
    )
    if (!($logs -match 'UPDATE_E2E_CONFIRMED')) { throw 'The real confirmation path did not execute.' }
    $logs | Where-Object { $_ -match 'UPDATE_E2E_(OFFER|CONFIRMED)' }
    "Portable E2E passed: older 6.5.3.180 -> published 6.5.3.181; SHA256=$($fixture.targetSha256); sentinel preserved; relaunched PID=$relaunchedPid"
}
finally {
    if ($process.Path -eq $exe -and !$process.HasExited) {
        Stop-Process -Id $relaunchedPid
    }
}
