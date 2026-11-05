@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > "tmp\launcher-hip-env.log" 2>&1
if errorlevel 1 exit /b 1
set "HIP_PLATFORM=amd"
set "HIP_PATH=D:\Projects\Strata\.rocm-win\Lib\site-packages\_rocm_sdk_devel"
set "ROCM_PATH=%HIP_PATH%"
set "PATH=%HIP_PATH%\bin;D:\Projects\Strata\.rocm-win\Lib\site-packages\_rocm_sdk_core\lib\llvm\bin;%PATH%"
cmake -S . -B build-cluster/host17-core -DSTRATA_BUILD_NATIVE_CLUSTER=ON > "tmp\launcher-hip-configure.log" 2>&1
if errorlevel 1 (
  type "tmp\launcher-hip-configure.log"
  exit /b 1
)
cmake --build build-cluster/host17-core --target strata-cluster --parallel 6 > "tmp\launcher-hip-build.log" 2>&1
set "build_result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content -LiteralPath tmp\launcher-hip-build.log -Tail 15"
exit /b %build_result%

