@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\native-server-cuda-env.log 2>&1
if errorlevel 1 exit /b 1
cmake -S . -B build-intel -DSTRATA_BUILD_NATIVE_CLUSTER=ON -DSTRATA_WEBP_DIR=D:/Projects/Strata/tmp/native-image-deps/libwebp-1.6.0 -DSTRATA_AVIF_DIR=D:/Projects/Strata/tmp/native-image-deps/libavif-1.4.2 -DSTRATA_AOM_DIR=D:/Projects/Strata/tmp/native-image-deps/aom-3.14.1 -DSTRATA_JSONCONS_DIR=D:/Projects/Strata/tmp/jsoncons-source/jsoncons-1.7.0 > tmp\native-server-cuda-configure.log 2>&1
if errorlevel 1 (
  type tmp\native-server-cuda-configure.log
  exit /b 1
)
cmake --build build-intel --target strata-server strata-cluster --parallel 6 > tmp\native-server-cuda-build.log 2>&1
set "result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content tmp/native-server-cuda-build.log -Tail 30"
exit /b %result%

