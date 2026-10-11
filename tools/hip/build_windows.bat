@echo off
rem Strata's HIP (AMD) engine for Windows, and the ready-made zip setup.py downloads (strata-windows-x64-hip.zip).
rem
rem   tools\hip\build_windows.bat              build strata.exe + strata-device.exe and package dist\strata-windows-x64-hip.zip
rem   tools\hip\build_windows.bat tests        also build the HIP unit tests (ctest in build-hip-win; they need an AMD GPU)
rem   tools\hip\build_windows.bat network      also build and package the LAN expert worker library and checker
rem   tools\hip\build_windows.bat native-network  C++ builder, unpacked SDK, output DIST_DIR/host17 (no Python)
rem
rem Needs: Visual Studio 2022 (or 2019) Build Tools with the C++ workload (the linker, the CRT and the Windows SDK),
rem Python 3.10+ and git.  No admin rights and no AMD GPU: ROCm comes from AMD's TheRock Python wheels (the same
rem ROCm 10.2 nightly #325 ran on an RX 9070 XT), installed into a private venv (ROCM_VENV, ~7 GB).  CMake refuses to
rem mix cl.exe with Clang for HIP, so ROCm's clang compiles the host code too (targeting the MSVC ABI, with MSVC's
rem headers and linker from vcvars); cmake/hip_backend.cmake force-includes the CUDA->HIP shim.
rem
rem Settings (environment variables, all optional):
rem   STRATA_HIP_ARCHS     gfx1100;gfx1101;gfx1102;gfx1200;gfx1201;gfx1030;gfx1031;gfx1151 (the cards setup supports, + gfx1102; gfx1151 = Strix Halo;
rem                        gfx1031 = RX 6700 XT / 6800M, #915 #1078; drop it from the list to build the zip without it)
rem   STRATA_ROCM_VERSION  10.2.0a20260930        STRATA_ROCM_INDEX  https://nightly.repo.amd.com/rocm/whl-next/
rem   ROCM_VENV            <repo>\.rocm-win       BUILD_DIR          <repo>\build-hip-win     DIST_DIR  <repo>\dist
rem   STRATA_GGML_DIR      a llama.cpp checkout at the pinned commit (default: CMake fetches it)
rem   STRATA_ROCM_COMPILER_ROOT  optional compiler root; SDK libraries still come from rocm-sdk path --root
rem   STRATA_BUILD_JOBS    compiler jobs, default 6
rem   STRATA_ROCM_SKIP_INSTALL=1  use installed SDK only; never create a venv or download packages
if /i "%~1"=="native-network" goto native_network
setlocal EnableDelayedExpansion
for %%I in ("%~dp0..\..") do set "SRC=%%~fI"
if not defined STRATA_HIP_ARCHS set "STRATA_HIP_ARCHS=gfx1100;gfx1101;gfx1102;gfx1200;gfx1201;gfx1030;gfx1031;gfx1151"
if not defined STRATA_ROCM_VERSION set "STRATA_ROCM_VERSION=10.2.0a20260930"
if not defined STRATA_ROCM_INDEX set "STRATA_ROCM_INDEX=https://nightly.repo.amd.com/rocm/whl-next/"
if not defined ROCM_VENV set "ROCM_VENV=%SRC%\.rocm-win"
if not defined BUILD_DIR set "BUILD_DIR=%SRC%\build-hip-win"
if not defined DIST_DIR set "DIST_DIR=%SRC%\dist"
if not defined STRATA_BUILD_JOBS set "STRATA_BUILD_JOBS=6"
set "TESTS=OFF"
set "NETWORK="
set "NATIVE_HOST=OFF"
if /i "%~1"=="tests" set "TESTS=ON"
if /i "%~1"=="network" set "NETWORK=--network"

rem ---- 1. ROCm (TheRock wheels: the compiler, the HIP runtime, hipBLAS/hipBLASLt/rocBLAS, a device package per arch)
set "PY="
py -3 -c "import sys" >nul 2>nul && set "PY=py -3"
if not defined PY python -c "import sys" >nul 2>nul && set "PY=python"
if not defined PY (echo Python 3.10+ is needed & exit /b 1)
if not exist "%ROCM_VENV%\Scripts\python.exe" (
  if "%STRATA_ROCM_SKIP_INSTALL%"=="1" (
    echo ROCm SDK is missing and installation is disabled: %ROCM_VENV%
    exit /b 1
  )
  %PY% -m venv "%ROCM_VENV%" || exit /b 1
)
set "EXTRAS=libraries,devel"
for %%A in (%STRATA_HIP_ARCHS:;= %) do set "EXTRAS=!EXTRAS!,device-%%A"
set "STAMP=%ROCM_VENV%\strata-rocm.txt"
set "WANT=%STRATA_ROCM_VERSION% %EXTRAS%"
set "HAVE="
if exist "%STAMP%" set /p HAVE=<"%STAMP%"
if not "!HAVE!"=="!WANT!" (
  if "%STRATA_ROCM_SKIP_INSTALL%"=="1" (
    echo Installed ROCm SDK does not match the requested version and architectures; installation is disabled.
    exit /b 1
  )
  echo Installing ROCm %STRATA_ROCM_VERSION% [%EXTRAS%] into %ROCM_VENV% ...
  "%ROCM_VENV%\Scripts\python.exe" -m pip install --disable-pip-version-check --cache-dir "%SRC%\tmp\hip-pip-cache" --index-url "%STRATA_ROCM_INDEX%" "rocm[%EXTRAS%]==%STRATA_ROCM_VERSION%" || exit /b 1
  "%ROCM_VENV%\Scripts\rocm-sdk.exe" init || exit /b 1
  >"%STAMP%" echo !WANT!
)
for /f "delims=" %%R in ('"%ROCM_VENV%\Scripts\rocm-sdk.exe" path --root') do set "ROCM=%%R"
if not defined STRATA_ROCM_COMPILER_ROOT set "STRATA_ROCM_COMPILER_ROOT=%ROCM%"
if not exist "%STRATA_ROCM_COMPILER_ROOT%\lib\llvm\bin\clang++.exe" (echo ROCm has no compiler in "%STRATA_ROCM_COMPILER_ROOT%" & exit /b 1)
set "ROCM_F=%ROCM:\=/%"
set "ROCM_COMPILER_F=%STRATA_ROCM_COMPILER_ROOT:\=/%"
set "BITCODE=%ROCM_F%/lib/llvm/amdgcn/bitcode"

rem ---- 2. MSVC: the linker, the C runtime and the Windows SDK (ROCm's clang uses them)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
if exist "%VSWHERE%" for /f "delims=" %%V in ('"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS=%%V"
if not defined VS (echo Visual Studio Build Tools with the C++ workload are needed & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "HIP_PLATFORM=amd"
set "HIP_PATH=%ROCM%"
set "ROCM_PATH=%ROCM%"
set "PATH=%ROCM%\bin;%STRATA_ROCM_COMPILER_ROOT%\lib\llvm\bin;%PATH%"

rem ---- 3. configure + build (STRATA_PORTABLE: the CPU kernels for an AVX2 baseline, as in the NVIDIA zip)
set "GGML="
if defined STRATA_GGML_DIR set "GGML=-DSTRATA_GGML_DIR=%STRATA_GGML_DIR:\=/%"
rem Always configure: a changed arch list or network mode must not reuse stale cached options.
cmake -G Ninja -S "%SRC%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release ^
    -DSTRATA_ENABLE_HIP=ON -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_SYCL=OFF -DSTRATA_HIP_GFX906=OFF -DSTRATA_BUILD_TESTS=%TESTS% -DSTRATA_PREFILL_MMQ=ON ^
    -DSTRATA_NATIVE_EXPERTS=ON -DSTRATA_PORTABLE=ON "-DCMAKE_HIP_ARCHITECTURES=%STRATA_HIP_ARCHS%" ^
    "-DCMAKE_C_COMPILER=%ROCM_COMPILER_F%/lib/llvm/bin/clang.exe" "-DCMAKE_CXX_COMPILER=%ROCM_COMPILER_F%/lib/llvm/bin/clang++.exe" ^
    "-DCMAKE_HIP_COMPILER=%ROCM_COMPILER_F%/lib/llvm/bin/clang++.exe" "-DCMAKE_HIP_COMPILER_ROCM_ROOT=%ROCM_F%" ^
    "-DCMAKE_PREFIX_PATH=%ROCM_F%" "-DCMAKE_HIP_FLAGS=--rocm-path=%ROCM_F% --rocm-device-lib-path=%BITCODE%" ^
    "-DCMAKE_C_FLAGS=-DWIN32 -D_WINDOWS" "-DCMAKE_CXX_FLAGS=-DWIN32 -D_WINDOWS -fexceptions -fcxx-exceptions" ^
    "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG" "-DCMAKE_HIP_FLAGS_RELEASE=-O3 -DNDEBUG" ^
    -DSTRATA_BUILD_NETWORK_BACKEND=ON -DSTRATA_BUILD_NATIVE_CLUSTER=%NATIVE_HOST% ^
    %GGML% || exit /b 1
if "%TESTS%"=="ON" (
  cmake --build "%BUILD_DIR%" --parallel %STRATA_BUILD_JOBS% || exit /b 1
) else (
  if "%NATIVE_HOST%"=="ON" (
    cmake --build "%BUILD_DIR%" --target strata strata-host strata_network_backend --parallel %STRATA_BUILD_JOBS% || exit /b 1
  ) else if defined NETWORK (
    cmake --build "%BUILD_DIR%" --target strata strata-device strata_network_backend strata-network-check --parallel %STRATA_BUILD_JOBS% || exit /b 1
  ) else (
    cmake --build "%BUILD_DIR%" --target strata strata-device --parallel %STRATA_BUILD_JOBS% || exit /b 1
  )
)

rem ---- 4. the zip: the two programs, the ROCm DLLs they load (+ rocBLAS/hipBLASLt kernels for these archs), licenses
"%ROCM_VENV%\Scripts\python.exe" "%SRC%\tools\hip\package_windows.py" --build "%BUILD_DIR%" --rocm "%ROCM%" ^
  --archs "%STRATA_HIP_ARCHS%" --rocm-version "%STRATA_ROCM_VERSION%" --out "%DIST_DIR%" %NETWORK% || exit /b 1
echo.
echo Done: %DIST_DIR%\strata-windows-x64-hip.zip
endlocal
exit /b 0

:native_network
setlocal EnableExtensions DisableDelayedExpansion
for %%I in ("%~dp0..\..") do set "SRC=%%~fI"
if not defined BUILD_DIR set "BUILD_DIR=%SRC%\build-cluster\host17"
if not defined DIST_DIR set "DIST_DIR=%SRC%\dist\cluster-native"
if not defined STRATA_BUILD_JOBS set "STRATA_BUILD_JOBS=6"
if not defined STRATA_HIP_ARCHS set "STRATA_HIP_ARCHS=gfx1102;gfx1103"
call "%SRC%\tools\build_cluster_native.bat" --hosts host17 --build-dir "host17=%BUILD_DIR%" --out-root "%DIST_DIR%" --jobs %STRATA_BUILD_JOBS% --hip-archs %STRATA_HIP_ARCHS:;= %
exit /b %errorlevel%
