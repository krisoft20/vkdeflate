@echo off
setlocal
cd /d %~dp0
set OUT=%~dp0..\..\third_party_build\gdref\build
call "%~dp0..\busy.cmd"
if errorlevel 1 exit /b 1
call "%~dp0..\vsenv.cmd"
if errorlevel 1 exit /b 1
if not exist "%OUT%\build.ninja" cmake -G Ninja -B "%OUT%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
if errorlevel 1 exit /b 1
cmake --build "%OUT%"
exit /b %errorlevel%
