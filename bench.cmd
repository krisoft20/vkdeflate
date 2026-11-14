@echo off
setlocal
cd /d %~dp0
rem bench.cmd        builds vkdeflate, fetches and compresses the Silesia corpus, decodes it on the GPU and
rem                  writes results\<gpu>.csv and results\<gpu>.txt
rem bench.cmd -khr   also builds Khronos's memory decompression layer and decodes every stream with it too
rem bench.cmd -prep  stops after the corpus and the streams, for check.cmd
rem Close other programs that use the GPU first: kernel times move by 10 % and more when it is shared.
rem The corpus is downloaded to test\silesia (66 MB); set SILESIA to a folder that already holds the twelve files.
rem The GDeflate reference encoder (Microsoft's DirectStorage repository with NVIDIA's libdeflate fork) and the
rem Khronos layer (Vulkan-ExtensionLayer, which needs python) are cloned and built in third_party_build, at the
rem commits in tools\third_party.sh.
rem In a release package, which has no CMakeLists.txt, bench and the encoder come built.
call "%~dp0tools\busy.cmd"
if errorlevel 1 exit /b 1
if not exist CMakeLists.txt goto :built
call "%~dp0build.cmd"
if errorlevel 1 exit /b 1
call :encoder
if errorlevel 1 exit /b 1
:built
call :corpus
if errorlevel 1 exit /b 1
call :vectors
if errorlevel 1 exit /b 1
if /i "%1"=="-prep" exit /b 0
if /i not "%1"=="-khr" goto :run
call :khr_layer
if errorlevel 1 exit /b 1
:run
set LAYERS=%~dp0third_party_build\Vulkan-ExtensionLayer\build\layers
if exist "%LAYERS%\VkLayer_khronos_memory_decompression.json" set VK_LAYER_PATH=%LAYERS%
"%~dp0build\bench.exe" -list test\vectors\silesia.txt -csvdir results
exit /b %errorlevel%

:corpus
set SIL=%cd%\test\silesia
if defined SILESIA set SIL=%SILESIA%
if exist "%SIL%\dickens" exit /b 0
if not exist test\silesia mkdir test\silesia
echo downloading the silesia corpus, 66 MB
curl -L --fail -o test\silesia.zip https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
if errorlevel 1 exit /b 1
"%SystemRoot%\System32\tar.exe" -xf test\silesia.zip -C test\silesia
if errorlevel 1 exit /b 1
del test\silesia.zip
exit /b 0

:encoder
if exist third_party_build\DirectStorage\GDeflate\3rdparty\libdeflate\libdeflate.h goto :build_encoder
echo cloning the gdeflate reference encoder
call "%~dp0tools\fetch.cmd" directstorage
if errorlevel 1 exit /b 1
:build_encoder
call "%~dp0tools\gdref\build.cmd"
exit /b %errorlevel%

:vectors
if not exist test\vectors mkdir test\vectors
del /q test\vectors\silesia.txt 2>nul
for %%f in (dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml) do call :vector %%f
if not exist test\vectors\silesia.txt exit /b 1
exit /b 0

:vector
if not exist test\vectors\%1.l6.gdz third_party_build\gdref\build\gdref.exe c "%SIL%\%1" test\vectors\%1.l6.gdz 6
if not exist test\vectors\%1.l6.gdz exit /b 1
echo test\vectors\%1.l6.gdz>>test\vectors\silesia.txt
exit /b 0

:khr_layer
if not exist third_party_build\Vulkan-ExtensionLayer\CMakeLists.txt call "%~dp0tools\fetch.cmd" layer
if errorlevel 1 exit /b 1
call "%~dp0tools\vsenv.cmd"
if errorlevel 1 exit /b 1
pushd third_party_build\Vulkan-ExtensionLayer
if not exist build\build.ninja cmake -S . -B build -G Ninja -D UPDATE_DEPS=ON -D CMAKE_BUILD_TYPE=Release -D BUILD_TESTS=OFF -D CMAKE_C_COMPILER=cl -D CMAKE_CXX_COMPILER=cl
if errorlevel 1 (
    popd
    exit /b 1
)
cmake --build build --target VkLayer_khronos_memory_decompression
set RC=%errorlevel%
popd
exit /b %RC%
