@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Build the two Intel nodes separately. host11 must never inherit host16's /QxHost flags.
rem Usage: tools\build_cluster_intel.bat host16 ^| host11
rem Optional: STRATA_ONEAPI_VARS, STRATA_CLUSTER_BUILD_DIR, STRATA_GGML_DIR, STRATA_BUILD_JOBS, STRATA_ICX_COMPILER.
if /i "%~1"=="host16" goto host16
if /i "%~1"=="host11" goto host11
echo Usage: tools\build_cluster_intel.bat host16 ^| host11
exit /b 2

:host16
set "STRATA_CLUSTER_DEFAULT_BUILD=build-intel"
set "STRATA_CLUSTER_CUDA=ON"
set "STRATA_CLUSTER_PORTABLE=OFF"
set "STRATA_CLUSTER_RELEASE=/O3 /DNDEBUG /QxHost /fp:precise"
set "STRATA_CLUSTER_TARGETS=strata strata-device strata_network_backend strata-network-check"
goto configure

:host11
set "STRATA_CLUSTER_DEFAULT_BUILD=build-network-cpu"
set "STRATA_CLUSTER_CUDA=OFF"
set "STRATA_CLUSTER_PORTABLE=ON"
set "STRATA_CLUSTER_RELEASE=/O3 /DNDEBUG /fp:precise"
set "STRATA_CLUSTER_TARGETS=strata_network_backend strata-network-check"

:configure
for %%I in ("%~dp0..") do set "STRATA_CLUSTER_ROOT=%%~fI"
if not defined STRATA_ONEAPI_VARS (
  if exist "%ProgramFiles(x86)%\Intel\oneAPI\setvars.bat" (
    set "STRATA_ONEAPI_VARS=%ProgramFiles(x86)%\Intel\oneAPI\setvars.bat"
  ) else (
    set "STRATA_ONEAPI_VARS=%ProgramFiles(x86)%\Intel\oneAPI\2026.1\oneapi-vars.bat"
  )
)
if not exist "%STRATA_ONEAPI_VARS%" (
  echo Set STRATA_ONEAPI_VARS to the installed Intel oneAPI environment batch file.
  exit /b 1
)
call "%STRATA_ONEAPI_VARS%" >nul 2>&1
if errorlevel 1 exit /b 1
if not defined STRATA_CLUSTER_BUILD_DIR set "STRATA_CLUSTER_BUILD_DIR=%STRATA_CLUSTER_ROOT%\%STRATA_CLUSTER_DEFAULT_BUILD%"
set "STRATA_CLUSTER_ICX=%STRATA_ICX_COMPILER%"
rem Keep an existing cache's compiler spelling (e.g. versioned path vs latest junction).
rem Changing that spelling can make CMake clear the cache and lose the requested portable build options.
if not defined STRATA_CLUSTER_ICX if exist "%STRATA_CLUSTER_BUILD_DIR%\CMakeCache.txt" (
  for /f "tokens=1,* delims==" %%I in ('findstr /b /c:"CMAKE_CXX_COMPILER:" "%STRATA_CLUSTER_BUILD_DIR%\CMakeCache.txt"') do set "STRATA_CLUSTER_ICX=%%J"
)
if not defined STRATA_CLUSTER_ICX for /f "delims=" %%I in ('where icx-cl.exe 2^>nul') do if not defined STRATA_CLUSTER_ICX set "STRATA_CLUSTER_ICX=%%I"
if not defined STRATA_CLUSTER_ICX (
  echo Intel icx-cl.exe was not found after initializing oneAPI.
  exit /b 1
)
if not defined VCToolsInstallDir (
  echo Visual Studio C++ build tools were not initialized by oneAPI.
  exit /b 1
)
if not defined STRATA_BUILD_JOBS set "STRATA_BUILD_JOBS=6"
set "STRATA_CLUSTER_GGML="
if defined STRATA_GGML_DIR set "STRATA_CLUSTER_GGML=-DSTRATA_GGML_DIR=%STRATA_GGML_DIR%"
echo Node: %~1
echo Build: %STRATA_CLUSTER_BUILD_DIR%
echo Release flags: %STRATA_CLUSTER_RELEASE%
echo Portable AVX2 baseline: %STRATA_CLUSTER_PORTABLE%
cmake -S "%STRATA_CLUSTER_ROOT%" -B "%STRATA_CLUSTER_BUILD_DIR%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=%STRATA_CLUSTER_ICX%" "-DCMAKE_CXX_COMPILER=%STRATA_CLUSTER_ICX%" ^
  "-DCMAKE_LINKER=%VCToolsInstallDir%bin/Hostx64/x64/link.exe" "-DCMAKE_AR=%VCToolsInstallDir%bin/Hostx64/x64/lib.exe" ^
  "-DCMAKE_C_FLAGS=" "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc" ^
  "-DCMAKE_C_FLAGS_RELEASE=%STRATA_CLUSTER_RELEASE%" "-DCMAKE_CXX_FLAGS_RELEASE=%STRATA_CLUSTER_RELEASE%" ^
  -DSTRATA_ENABLE_CUDA=%STRATA_CLUSTER_CUDA% -DSTRATA_ENABLE_HIP=OFF -DSTRATA_ENABLE_SYCL=OFF ^
  -DSTRATA_PORTABLE=%STRATA_CLUSTER_PORTABLE% -DSTRATA_BUILD_TESTS=OFF -DSTRATA_BUILD_NETWORK_BACKEND=ON ^
  -DCMAKE_CUDA_ARCHITECTURES=89 -DCMAKE_CUDA_HOST_COMPILER=cl.exe "%STRATA_CLUSTER_GGML%"
if errorlevel 1 exit /b 1
cmake --build "%STRATA_CLUSTER_BUILD_DIR%" --target %STRATA_CLUSTER_TARGETS% --parallel %STRATA_BUILD_JOBS%
exit /b %errorlevel%
