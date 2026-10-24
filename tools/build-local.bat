@echo off
rem Local Windows x64 build of whatever branch is checked out in the target
rem checkout (default: the main checkout, normally on "integration").
rem Usage: build-local.bat [release|debug] [checkout-dir]
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=release
set SRC=%~2
if "%SRC%"=="" set SRC=E:\Code\moonlight-qt

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath`) do set VSROOT=%%i
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

rem E:\Qt\Tools\shim holds a no-op 7z.bat: local builds skip the zip packaging
set PATH=E:\Qt\6.12.0\msvc2022_64\bin;E:\Qt\Tools\jom;E:\Qt\Tools\shim;%PATH%
set CL=/MP
cd /d "%SRC%" || exit /b 1
call scripts\build-arch.bat %CONFIG% x64
