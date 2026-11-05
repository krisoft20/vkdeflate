@echo off
setlocal
cd /d %~dp0..
rem Builds Khronos's VK_LAYER_KHRONOS_memory_decompression (Vulkan-ExtensionLayer, Apache-2.0 or MIT) into
rem third_party_build, for bench. Needs git and python on the path: the repository fetches its own dependencies.
if exist third_party_build\Vulkan-ExtensionLayer\CMakeLists.txt goto :have_source
git clone --depth 1 https://github.com/KhronosGroup/Vulkan-ExtensionLayer third_party_build\Vulkan-ExtensionLayer
if errorlevel 1 exit /b 1
:have_source
call "%~dp0busy.cmd"
if errorlevel 1 exit /b 1
call "%~dp0vsenv.cmd"
if errorlevel 1 exit /b 1
cd third_party_build\Vulkan-ExtensionLayer
if not exist build\build.ninja cmake -S . -B build -G Ninja -D UPDATE_DEPS=ON -D CMAKE_BUILD_TYPE=Release -D BUILD_TESTS=OFF -D CMAKE_C_COMPILER=cl -D CMAKE_CXX_COMPILER=cl
if errorlevel 1 exit /b 1
cmake --build build --target VkLayer_khronos_memory_decompression
exit /b %errorlevel%
