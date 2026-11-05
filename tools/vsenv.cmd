@echo off
rem Puts cl, cmake and ninja from the newest Visual Studio on the path of the calling script.
if defined VKDEFLATE_VSENV exit /b 0
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set VSDIR=
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (
    echo visual studio with the c++ tools was not found
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
set VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake
if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"
set VKDEFLATE_VSENV=1
exit /b 0
