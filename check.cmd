@echo off
setlocal
cd /d %~dp0
rem check.cmd   builds vkdeflate and runs on your GPU what a change to src\ has to pass before it goes in: the
rem             twelve Silesia streams and a fuzz run with both literal-round settings, the attach path, the single
rem             header, the C interface, the layer and load(). Prepares the streams the way bench.cmd does.
rem CI has no GPU and runs none of this.
python tools\amalgamate.py --check
if errorlevel 1 exit /b 1
call "%~dp0bench.cmd" -prep
if errorlevel 1 exit /b 1
set SIL=%cd%\test\silesia
if defined SILESIA set SIL=%SILESIA%
set B=%~dp0build
"%B%\gdz.exe" -list test\vectors\silesia.txt -gpu -lits 0 || goto :failed
"%B%\gdz.exe" -list test\vectors\silesia.txt -gpu -lits 1 || goto :failed
"%B%\gdz.exe" -fuzz test\vectors\xml.l6.gdz -n 1000 -gpu -lits 0 || goto :failed
"%B%\gdz.exe" -fuzz test\vectors\xml.l6.gdz -n 1000 -gpu -lits 1 || goto :failed
"%B%\attach_test.exe" test\vectors\xml.l6.gdz "%SIL%\xml" || goto :failed
"%B%\single_test.exe" test\vectors\xml.l6.gdz || goto :failed
"%B%\c_test.exe" "%SIL%\xml" || goto :failed
set VK_LAYER_PATH=%B%\layer
"%B%\layer_test.exe" test\vectors\xml.l6.gdz || goto :failed
"%B%\example_load.exe" test\vectors\mozilla.l6.gdz || goto :failed
echo check: all passed
exit /b 0

:failed
echo check: failed
exit /b 1
