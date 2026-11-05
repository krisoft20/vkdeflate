@echo off
setlocal
rem Produces the test vectors with the GDeflate reference encoder (third_party_build\gdref, see README.md)
rem and writes test\vectors\manifest.txt for "gdz -list". Edit the three paths for another machine.
cd /d %~dp0

set GDREF=%~dp0..\third_party_build\gdref\build\gdref.exe
set SILESIA=E:\zbench\silesia
set CLIO=D:\Projects\games\clio\data
set OUT=%~dp0vectors
set CLIO_FILES=%CLIO%\map\climate.bin %CLIO%\map\provinces.bin %CLIO%\fonts\garamond.font %CLIO%\history\1241\settlements\villages.txt %CLIO%\buildings\surfaces_albedo.tex %CLIO%\textures\terrain_albedo.tex %CLIO%\gui\skin.tex %CLIO%\portraits\human.portrait %CLIO%\sounds\amb\sea\fs_straget_big_waves_hit_land.snd

call "%~dp0..\tools\busy.cmd"
if errorlevel 1 exit /b 1
if not exist "%GDREF%" (
    echo %GDREF% not found, build it first
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
del /q "%OUT%\manifest.txt" 2>nul

for %%f in (dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml) do (
    call :vec "%SILESIA%\%%f" %%f 6
)
for %%f in (dickens xml x-ray sao mr) do (
    for %%l in (0 1 3 9 12) do call :vec "%SILESIA%\%%f" %%f %%l
)
for %%f in (%CLIO_FILES%) do call :vec "%%f" %%~nxf 6
goto :eof

:vec
rem %1 source file, %2 name, %3 level
"%GDREF%" c %1 "%OUT%\%2.l%3.gdz" %3
if errorlevel 1 exit /b 1
echo %OUT%\%2.l%3.gdz %~1>>"%OUT%\manifest.txt"
goto :eof
