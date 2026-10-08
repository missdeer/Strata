@echo off
setlocal
cd /d D:\Projects\Strata
set "HIPVERIFY_ROCM="
for /f "delims=" %%R in ('.rocm-win\Scripts\rocm-sdk.exe path --root') do set "HIPVERIFY_ROCM=%%R"
if not defined HIPVERIFY_ROCM exit /b 1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "HIP_PLATFORM=amd"
set "HIP_PATH=%HIPVERIFY_ROCM%"
set "ROCM_PATH=%HIPVERIFY_ROCM%"
set "PATH=%HIPVERIFY_ROCM%\bin;%HIPVERIFY_ROCM%\lib\llvm\bin;%PATH%"
set "HIPVERIFY_ROCM_F=%HIPVERIFY_ROCM:\=/%"
cmake -G Ninja -S . -B build-cluster/host17 -DCMAKE_BUILD_TYPE=Release -DSTRATA_ENABLE_HIP=ON -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_SYCL=OFF -DSTRATA_BUILD_TESTS=OFF -DSTRATA_PREFILL_MMQ=ON -DSTRATA_NATIVE_EXPERTS=ON -DSTRATA_PORTABLE=ON -DSTRATA_BUILD_NETWORK_BACKEND=ON "-DCMAKE_HIP_ARCHITECTURES=gfx1102;gfx1103" "-DCMAKE_C_COMPILER=%HIPVERIFY_ROCM_F%/lib/llvm/bin/clang.exe" "-DCMAKE_CXX_COMPILER=%HIPVERIFY_ROCM_F%/lib/llvm/bin/clang++.exe" "-DCMAKE_HIP_COMPILER=%HIPVERIFY_ROCM_F%/lib/llvm/bin/clang++.exe" "-DCMAKE_HIP_COMPILER_ROCM_ROOT=%HIPVERIFY_ROCM_F%" "-DCMAKE_PREFIX_PATH=%HIPVERIFY_ROCM_F%" "-DCMAKE_HIP_FLAGS=--rocm-path=%HIPVERIFY_ROCM_F% --rocm-device-lib-path=%HIPVERIFY_ROCM_F%/lib/llvm/amdgcn/bitcode" "-DCMAKE_C_FLAGS=-DWIN32 -D_WINDOWS" "-DCMAKE_CXX_FLAGS=-DWIN32 -D_WINDOWS -fexceptions -fcxx-exceptions" "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG" "-DCMAKE_HIP_FLAGS_RELEASE=-O3 -DNDEBUG" "-DSTRATA_GGML_DIR=D:/Projects/Strata/build-intel/_deps/strata_llamacpp-src" >tmp/hip-sdk-configure.log 2>&1
set "HIPVERIFY_RESULT=%errorlevel%"
type tmp\hip-sdk-configure.log
exit /b %HIPVERIFY_RESULT%
