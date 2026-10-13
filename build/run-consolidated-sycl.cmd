@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\consolidated-sycl-sycl-env.log 2>&1
if errorlevel 1 exit /b 1
cmake -S sycl -B build-sycl-text -G Ninja "-DCMAKE_CXX_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" "-DCMAKE_C_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" -DSTRATA_GGML_DIR=D:/Projects/Strata/third_party/llama.cpp -DSTRATA_BUILD_NATIVE_TEXT=ON -DSTRATA_BUILD_NATIVE_SERVER=ON -DSTRATA_WEBP_DIR=D:/Projects/Strata/tmp/native-image-deps/libwebp-1.6.0 -DSTRATA_AVIF_DIR=D:/Projects/Strata/tmp/native-image-deps/libavif-1.4.2 -DSTRATA_AOM_DIR=D:/Projects/Strata/tmp/native-image-deps/aom-3.14.1 -DSTRATA_JSONCONS_DIR=D:/Projects/Strata/tmp/jsoncons-source/jsoncons-1.7.0 > tmp\consolidated-sycl-sycl-configure.log 2>&1
if errorlevel 1 (
  powershell.exe -NoProfile -Command "Get-Content tmp/consolidated-sycl-sycl-configure.log -Tail 45"
  exit /b 1
)
cmake --build build-sycl-text --target strata-host --parallel 6 > tmp\consolidated-sycl-sycl-build.log 2>&1
set "result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content tmp/consolidated-sycl-sycl-build.log -Tail 30"
exit /b %result%

