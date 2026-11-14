@echo off
rem tools\fetch.cmd directstorage|layer   clones Microsoft's DirectStorage (with NVIDIA's libdeflate) or Khronos's
rem Vulkan-ExtensionLayer into third_party_build at the commit named in tools\third_party.sh
setlocal
cd /d %~dp0..
for /f "eol=# tokens=1,2 delims==" %%a in (tools\third_party.sh) do set %%a=%%b
if /i "%1"=="directstorage" goto :ds
if /i "%1"=="layer" goto :layer
echo usage: tools\fetch.cmd directstorage^|layer
exit /b 1

:ds
call :fetch third_party_build\DirectStorage https://github.com/microsoft/DirectStorage %DIRECTSTORAGE%
if errorlevel 1 exit /b 1
git -C third_party_build\DirectStorage submodule update --init --depth 1 GDeflate/3rdparty/libdeflate
exit /b %errorlevel%

:layer
call :fetch third_party_build\Vulkan-ExtensionLayer https://github.com/KhronosGroup/Vulkan-ExtensionLayer %EXTENSION_LAYER%
exit /b %errorlevel%

:fetch
if not exist %1\.git (
    git init -q %1 || exit /b 1
    git -C %1 remote add origin %2 || exit /b 1
)
git -C %1 fetch -q --depth 1 origin %3 || exit /b 1
git -C %1 checkout -q FETCH_HEAD
exit /b %errorlevel%
