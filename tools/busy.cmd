@echo off
rem Loads local.cmd from the repository root when it exists (git ignores it; it may set VKDEFLATE_CMAKE_ARGS and
rem VKDEFLATE_BUSY), then refuses to start while the program named in VKDEFLATE_BUSY is running, for example a game.
if exist "%~dp0..\local.cmd" call "%~dp0..\local.cmd"
if not defined VKDEFLATE_BUSY exit /b 0
tasklist /fi "imagename eq %VKDEFLATE_BUSY%" 2>nul | "%SystemRoot%\System32\find.exe" /i "%VKDEFLATE_BUSY%" >nul
if errorlevel 1 exit /b 0
echo %VKDEFLATE_BUSY% is running, not starting
exit /b 1
