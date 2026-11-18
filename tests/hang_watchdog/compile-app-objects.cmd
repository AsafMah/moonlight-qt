@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
set "PATH=C:\tmp\qt\6.12.0\msvc2022_64\bin;C:\tmp\qt\jom;C:\Program Files\Python315;%PATH%"
set "CL=/MP1"
cd /d "%~dp0..\..\build\build-x64-release\app"
jom -j4 -f Makefile.Release release\main.obj release\session.obj release\ffmpeg.obj release\d3d11va.obj release\d3d11vrr.obj
if errorlevel 1 exit /b %errorlevel%
cl /c /nologo /std:c++17 /EHsc /O2 /MD /DUNICODE /D_UNICODE /DWIN32 /DWIN64 /Fo"..\..\hang-watchdog-receipts\production-native.obj" "%~dp0..\..\app\hangwatchdog.cpp"
exit /b %errorlevel%
