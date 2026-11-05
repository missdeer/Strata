@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Native cluster build: use an unpacked SDK. Never run Python, rocm-sdk or pip.
for %%I in ("%~dp0..") do set "SRC=%%~fI"
if not defined STRATA_ROCM_ROOT (
  echo Set STRATA_ROCM_ROOT or pass --rocm-root to an unpacked ROCm SDK.
  exit /b 1
)
if not defined STRATA_ROCM_COMPILER_ROOT set "STRATA_ROCM_COMPILER_ROOT=%STRATA_ROCM_ROOT%"
if not exist "%STRATA_ROCM_COMPILER_ROOT%\lib\llvm\bin\clang++.exe" (
  echo ROCm SDK compiler is missing; SDK installation is not performed by the native builder.
  exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio C++ build tools are required. & exit /b 1)
for /f "delims=" %%I in ('"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS=%%I"
if not defined VS (echo Visual Studio C++ build tools are required. & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 1
set "HIP_PLATFORM=amd"
set "HIP_PATH=%STRATA_ROCM_ROOT%"
set "ROCM_PATH=%STRATA_ROCM_ROOT%"
set "STRATA_ROCM_ROOT_F=%STRATA_ROCM_ROOT:\=/%"
set "STRATA_ROCM_COMPILER_F=%STRATA_ROCM_COMPILER_ROOT:\=/%"
set "PATH=%STRATA_ROCM_ROOT%\bin;%STRATA_ROCM_COMPILER_ROOT%\lib\llvm\bin;%PATH%"
if not defined STRATA_CLUSTER_BUILD_DIR set "STRATA_CLUSTER_BUILD_DIR=%SRC%\build-cluster\host17"
if not defined STRATA_BUILD_JOBS set "STRATA_BUILD_JOBS=6"
if not defined STRATA_HIP_ARCHS set "STRATA_HIP_ARCHS=gfx1102;gfx1103"
set "GGML="
if defined STRATA_GGML_DIR set "GGML=-DSTRATA_GGML_DIR=%STRATA_GGML_DIR%"
set "JSONCONS="
set "WEBP="
set "AVIF="
set "AOM="
if defined STRATA_JSONCONS_DIR set "JSONCONS=-DSTRATA_JSONCONS_DIR=%STRATA_JSONCONS_DIR%"
if defined STRATA_WEBP_DIR set "WEBP=-DSTRATA_WEBP_DIR=%STRATA_WEBP_DIR%"
if defined STRATA_AVIF_DIR set "AVIF=-DSTRATA_AVIF_DIR=%STRATA_AVIF_DIR%"
if defined STRATA_AOM_DIR set "AOM=-DSTRATA_AOM_DIR=%STRATA_AOM_DIR%"
cmake -S "%SRC%" -B "%STRATA_CLUSTER_BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_C_COMPILER=%STRATA_ROCM_COMPILER_F%/lib/llvm/bin/clang.exe" ^
  "-DCMAKE_CXX_COMPILER=%STRATA_ROCM_COMPILER_F%/lib/llvm/bin/clang++.exe" ^
  "-DCMAKE_HIP_COMPILER=%STRATA_ROCM_COMPILER_F%/lib/llvm/bin/clang++.exe" ^
  "-DCMAKE_HIP_COMPILER_ROCM_ROOT=%STRATA_ROCM_ROOT_F%" "-DCMAKE_PREFIX_PATH=%STRATA_ROCM_ROOT_F%" ^
  "-DCMAKE_HIP_FLAGS=--rocm-path=%STRATA_ROCM_ROOT_F% --rocm-device-lib-path=%STRATA_ROCM_ROOT_F%/lib/llvm/amdgcn/bitcode" ^
  "-DCMAKE_C_FLAGS=-DWIN32 -D_WINDOWS" "-DCMAKE_CXX_FLAGS=-DWIN32 -D_WINDOWS -fexceptions -fcxx-exceptions" ^
  "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG" "-DCMAKE_HIP_FLAGS_RELEASE=-O3 -DNDEBUG" ^
  "-DCMAKE_HIP_ARCHITECTURES=%STRATA_HIP_ARCHS%" -DSTRATA_ENABLE_HIP=ON -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_SYCL=OFF ^
  -DSTRATA_HIP_GFX906=OFF -DSTRATA_PORTABLE=ON -DSTRATA_BUILD_TESTS=OFF -DSTRATA_BUILD_NETWORK_BACKEND=ON ^
  -DSTRATA_BUILD_NATIVE_CLUSTER=ON -DSTRATA_PREFILL_MMQ=ON -DSTRATA_NATIVE_EXPERTS=ON "%GGML%" "%JSONCONS%" "%WEBP%" "%AVIF%" "%AOM%"
if errorlevel 1 exit /b 1
cmake --build "%STRATA_CLUSTER_BUILD_DIR%" --target strata strata-host strata_network_backend --parallel %STRATA_BUILD_JOBS%
exit /b %errorlevel%
