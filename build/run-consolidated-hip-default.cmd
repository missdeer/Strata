@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > tmp\consolidated-hip-default-env.log 2>&1
if errorlevel 1 exit /b 1
set "HIP_PLATFORM=amd"
set "HIP_PATH=D:\Projects\Strata\.rocm-win\Lib\site-packages\_rocm_sdk_devel"
set "ROCM_PATH=%HIP_PATH%"
set "PATH=%HIP_PATH%\bin;D:\Projects\Strata\.rocm-win\Lib\site-packages\_rocm_sdk_core\lib\llvm\bin;%PATH%"
cmake -S . -B build-cluster/host17-core -DSTRATA_BUILD_NATIVE_CLUSTER=OFF -DSTRATA_WEBP_DIR=D:/Projects/Strata/tmp/native-image-deps/libwebp-1.6.0 -DSTRATA_AVIF_DIR=D:/Projects/Strata/tmp/native-image-deps/libavif-1.4.2 -DSTRATA_AOM_DIR=D:/Projects/Strata/tmp/native-image-deps/aom-3.14.1 -DSTRATA_JSONCONS_DIR=D:/Projects/Strata/tmp/jsoncons-source/jsoncons-1.7.0 > tmp\consolidated-hip-default-configure.log 2>&1
if errorlevel 1 (
  type tmp\consolidated-hip-default-configure.log
  exit /b 1
)
cmake --build build-cluster/host17-core --target strata --parallel 6 > tmp\consolidated-hip-default-build.log 2>&1
set "result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content tmp/consolidated-hip-default-build.log -Tail 30"
exit /b %result%

