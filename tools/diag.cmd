@echo off
setlocal
cd /d %~dp0..
rem Times the decoder with each kernel switch on five streams and saves what the shader compiler made of the
rem kernel, for comparing GPUs and compilers. Run bench.cmd first: it builds the tools and the streams.
rem Writes results\diag\diag.txt and results\diag\isa_x*.txt. Takes about two minutes.
call "%~dp0busy.cmd"
if errorlevel 1 exit /b 1
if not exist build\gdz.exe goto :need_bench
if not exist test\vectors\dickens.l6.gdz goto :need_bench
if not exist third_party_build\gdref\build\gdref.exe goto :need_bench
set FINDSTR=%SystemRoot%\System32\findstr.exe
set D=results\diag
if not exist %D% mkdir %D%
set OUT=%D%\diag.txt
del /q %OUT% 2>nul

if exist test\vectors\one_tile.gdz goto :have_one
build\gdz.exe test\vectors\dickens.l6.gdz -o test\vectors\one_tile.all >nul
if errorlevel 1 exit /b 1
powershell -NoProfile -Command "$b = [IO.File]::ReadAllBytes('test\vectors\one_tile.all'); [IO.File]::WriteAllBytes('test\vectors\one_tile.bin', [byte[]]$b[0..65535])"
if errorlevel 1 exit /b 1
third_party_build\gdref\build\gdref.exe c test\vectors\one_tile.bin test\vectors\one_tile.gdz 6 >nul
if errorlevel 1 exit /b 1
del /q test\vectors\one_tile.all test\vectors\one_tile.bin
:have_one

for %%x in (0 30 31 32) do call :stats %%x
>>%OUT% echo --- kernel ms, best of 200 rounds after 50 warm-up rounds
for %%s in (one_tile dickens.l6 osdb.l6 nci.l6 mozilla.l6) do call :stream %%s
type %OUT%
echo.
echo written to %D%, please send the whole folder
exit /b 0

:stats
>>%OUT% echo --- compiler statistics and a checked decode, -x %1
build\gdz.exe test\vectors\mozilla.l6.gdz -gpu -v -x %1 | %FINDSTR% /l /c:"[vkdeflate]" /c:"matches" /c:"FAIL" >>%OUT%
copy /y vkdeflate_tile_0.txt %D%\isa_x%1.txt >nul
exit /b 0

:stream
call :one %1 "-x 0"
call :one %1 "-x 30"
call :one %1 "-x 31"
call :one %1 "-x 32"
call :one %1 "-root 10"
call :one %1 "-x 22 -ring 4"
call :one %1 "-x 23"
call :one %1 "-x 11"
exit /b 0

:one
for /f "tokens=4" %%t in ('build\gdz.exe test\vectors\%1.gdz -gpu -nocpu -rounds 200 -warm 50 %~2 ^| %FINDSTR% /l /c:"gpu kernel"') do >>%OUT% echo %1  [%~2]  %%t
exit /b 0

:need_bench
echo run bench.cmd first
exit /b 1
