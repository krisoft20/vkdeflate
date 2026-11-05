@echo off
setlocal
cd /d %~dp0
rem bench.cmd        builds vkdeflate, fetches and compresses the Silesia corpus, decodes it on the GPU and
rem                  writes results\<gpu>.csv
rem bench.cmd -khr   also builds Khronos's memory decompression layer and decodes every stream with it too
rem Close other programs that use the GPU first: kernel times move by 10 % and more when it is shared.
call "%~dp0tools\busy.cmd"
if errorlevel 1 exit /b 1
call "%~dp0build.cmd"
if errorlevel 1 exit /b 1
call "%~dp0tools\data.cmd"
if errorlevel 1 exit /b 1
if /i not "%1"=="-khr" goto :run
call "%~dp0tools\khr_layer.cmd"
if errorlevel 1 exit /b 1
:run
set LAYERS=%~dp0third_party_build\Vulkan-ExtensionLayer\build\layers
if exist "%LAYERS%\VkLayer_khronos_memory_decompression.json" set VK_LAYER_PATH=%LAYERS%
"%~dp0build\bench.exe" -list test\vectors\silesia.txt -csvdir results
exit /b %errorlevel%
