@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\native-text-sycl-env.log 2>&1
if errorlevel 1 exit /b 1
cmake -S sycl -B build-sycl-text -G Ninja "-DCMAKE_CXX_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" "-DCMAKE_C_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" -DSTRATA_GGML_DIR=D:/Projects/Strata/third_party/llama.cpp -DSTRATA_BUILD_NATIVE_TEXT=ON -DSTRATA_JSONCONS_DIR=D:/Projects/Strata/tmp/jsoncons-source/jsoncons-1.7.0 > tmp\native-text-sycl-configure.log 2>&1
if errorlevel 1 (
  powershell.exe -NoProfile -Command "Get-Content tmp/native-text-sycl-configure.log -Tail 45"
  exit /b 1
)
cmake --build build-sycl-text --target strata-text-check --parallel 6 > tmp\native-text-sycl-build.log 2>&1
set "result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content tmp/native-text-sycl-build.log -Tail 30"
exit /b %result%
