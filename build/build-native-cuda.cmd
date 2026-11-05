@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > "tmp\native-cuda-env.log" 2>&1
if errorlevel 1 exit /b 1
cmake -S . -B build-intel -DSTRATA_BUILD_NATIVE_CLUSTER=ON > "tmp\native-cuda-configure.log" 2>&1
if errorlevel 1 (
  type "tmp\native-cuda-configure.log"
  exit /b 1
)
cmake --build build-intel --target strata-network-worker strata_network_backend strata-network-check --parallel 6 > "tmp\native-cuda-build.log" 2>&1
set "build_result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content -LiteralPath tmp\native-cuda-build.log -Tail 15"
exit /b %build_result%
