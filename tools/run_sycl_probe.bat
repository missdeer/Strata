@echo off
setlocal
rem Optional: set STRATA_ONEAPI_ENV to your oneAPI environment .bat file.
if not defined STRATA_ONEAPI_ENV set "STRATA_ONEAPI_ENV=C:\Program Files (x86)\Intel\oneAPI\setvars.bat"
if not exist "%STRATA_ONEAPI_ENV%" (
  echo Cannot find oneAPI environment script: "%STRATA_ONEAPI_ENV%"
  echo Set STRATA_ONEAPI_ENV to its full path and run again.
  exit /b 1
)
call "%STRATA_ONEAPI_ENV%"
if errorlevel 1 exit /b 1
where icx >nul 2>nul
if errorlevel 1 (
  echo icx was not found after initializing oneAPI.
  exit /b 1
)
set "PROBE_ROOT=%~dp0.."
set "PROBE_BUILD=%PROBE_ROOT%\build-sycl-probe"
if not exist "%PROBE_BUILD%" mkdir "%PROBE_BUILD%"
if errorlevel 1 exit /b 1
pushd "%PROBE_BUILD%"
if errorlevel 1 exit /b 1
icx -fsycl /std:c++17 /EHsc /O2 "%PROBE_ROOT%\tools\sycl_probe.cpp" /Fe:sycl_probe.exe
if errorlevel 1 (
  popd
  exit /b 1
)
sycl_probe.exe
set "PROBE_RESULT=%ERRORLEVEL%"
popd
exit /b %PROBE_RESULT%
