param(
    [Parameter(Mandatory)][string]$Exe,
    [string]$Platform,
    [string]$QtBin,
    [Parameter(ValueFromRemainingArguments)][string[]]$TestArgs
)

$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $Exe).Path
if (-not $QtBin) {
    $qmake = (Get-Command qmake -ErrorAction Stop).Source
    $QtBin = & $qmake -query QT_INSTALL_BINS
    if ($LASTEXITCODE -ne 0) { throw 'Could not locate the Qt runtime directory' }
}
if (-not (Test-Path -LiteralPath (Join-Path $QtBin 'Qt6Core.dll'))) {
    throw "Qt6Core.dll not found in $QtBin"
}

# Loader failures must be exit codes, not modal dialogs on a shared desktop.
Add-Type -Namespace MoonlightTests -Name ErrorMode -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);
[DllImport("kernel32.dll")] public static extern uint GetErrorMode();
'@
$null = [MoonlightTests.ErrorMode]::SetErrorMode(0x8003)
if (([MoonlightTests.ErrorMode]::GetErrorMode() -band 0x8003) -ne 0x8003) {
    throw 'Could not suppress native error dialogs; refusing to launch the test'
}
$env:PATH = "$(Split-Path -Parent $exePath);$QtBin;$env:PATH"
$env:QT_FORCE_STDERR_LOGGING = '1'
if ($Platform) { $env:QT_QPA_PLATFORM = $Platform }
Push-Location (Split-Path -Parent $exePath)
try {
    & $exePath @TestArgs 2>&1 | ForEach-Object { "$_" }
    $result = $LASTEXITCODE
}
finally {
    Pop-Location
}
"exit=$result (0x{0:X8})" -f [int]$result
exit $result
