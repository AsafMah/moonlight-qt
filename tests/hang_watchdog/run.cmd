@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cd /d "%~dp0"
set "PATH=C:\tmp\qt\6.12.0\msvc2022_64\bin;C:\tmp\qt\jom;C:\Program Files\PowerShell\7;%~dp0..\..\libs\windows\lib\x64;%PATH%"
set "CL=/MP1"
set "CMAKE_BUILD_PARALLEL_LEVEL=4"
qmake hang_watchdog.pro CONFIG+=release %*
if errorlevel 1 exit /b %errorlevel%
rem qmake/nmake do not invalidate objects when only DEFINES change.
rem Never let a quick-mode object masquerade as a production-12s test.
jom -j4 -f Makefile.Release clean
if errorlevel 1 exit /b %errorlevel%
jom -j4
if errorlevel 1 exit /b %errorlevel%
pwsh -NoProfile -File ..\..\scripts\run-built-test.ps1 -Exe release\hang_watchdog.exe -Platform offscreen
exit /b %errorlevel%
