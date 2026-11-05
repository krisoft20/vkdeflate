@echo off
setlocal
cd /d %~dp0
call "%~dp0tools\busy.cmd"
if errorlevel 1 exit /b 1
call "%~dp0tools\vsenv.cmd"
if errorlevel 1 exit /b 1
set CONFIGURE=
if not exist build\build.ninja set CONFIGURE=1
if defined VKDEFLATE_CMAKE_ARGS set CONFIGURE=1
if defined CONFIGURE cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl %VKDEFLATE_CMAKE_ARGS%
if errorlevel 1 exit /b 1
cmake --build build %1 %2 %3
exit /b %errorlevel%
