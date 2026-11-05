@echo off
setlocal
cd /d %~dp0..
rem The benchmark data: the Silesia corpus, compressed at level 6 with the GDeflate reference encoder
rem (Microsoft's DirectStorage repository with NVIDIA's libdeflate fork, cloned into third_party_build).
rem Set SILESIA to a folder that already holds the twelve files to skip the 66 MB download.
set SIL=%cd%\test\silesia
if defined SILESIA set SIL=%SILESIA%
if exist "%SIL%\dickens" goto :have_corpus
if not exist test\silesia mkdir test\silesia
echo downloading the silesia corpus, 66 MB
curl -L --fail -o test\silesia.zip https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
if errorlevel 1 exit /b 1
tar -xf test\silesia.zip -C test\silesia
if errorlevel 1 exit /b 1
del test\silesia.zip
:have_corpus

if exist third_party_build\DirectStorage\GDeflate\3rdparty\libdeflate\libdeflate.h goto :have_encoder
echo cloning the gdeflate reference encoder
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/microsoft/DirectStorage third_party_build\DirectStorage
if errorlevel 1 exit /b 1
:have_encoder
call "%~dp0gdref\build.cmd"
if errorlevel 1 exit /b 1

if not exist test\vectors mkdir test\vectors
del /q test\vectors\silesia.txt 2>nul
for %%f in (dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml) do call :vec %%f
if not exist test\vectors\silesia.txt exit /b 1
exit /b 0

:vec
if not exist test\vectors\%1.l6.gdz third_party_build\gdref\build\gdref.exe c "%SIL%\%1" test\vectors\%1.l6.gdz 6
if not exist test\vectors\%1.l6.gdz exit /b 1
echo test\vectors\%1.l6.gdz>>test\vectors\silesia.txt
exit /b 0
