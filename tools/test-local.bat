@echo off
rem Build and run one qmake unit test locally.
rem Usage: test-local.bat <test_dir> <target_exe> [checkout-dir]
setlocal
set TESTDIR=%~1
set TARGET=%~2
set SRC=%~3
if "%SRC%"=="" set SRC=E:\Code\moonlight-qt

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath`) do set VSROOT=%%i
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=E:\Qt\6.12.0\msvc2022_64\bin;E:\Qt\Tools\jom;%PATH%
set QT_QPA_PLATFORM=offscreen

cd /d "%SRC%\tests\%TESTDIR%" || exit /b 1
qmake %TESTDIR%.pro || exit /b 1
jom release || exit /b 1
if exist release\%TARGET%.exe (
    release\%TARGET%.exe
) else (
    %TARGET%.exe
)
