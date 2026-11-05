@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Lightweight bootstrap. The development builder is never included in node packages.
for %%I in ("%~dp0..") do set "STRATA_BUILDER_ROOT=%%~fI"
if not defined STRATA_BUILDER_DIR set "STRATA_BUILDER_DIR=%STRATA_BUILDER_ROOT%\build-cluster\builder"
if not exist "%STRATA_BUILDER_DIR%\strata-cluster-build.exe" (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (echo Visual Studio C++ build tools are required. & exit /b 1)
  for /f "delims=" %%I in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do call "%%I\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
  if errorlevel 1 exit /b 1
  cmake -S "%STRATA_BUILDER_ROOT%\tools\cluster_build" -B "%STRATA_BUILDER_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release
  if errorlevel 1 exit /b 1
  cmake --build "%STRATA_BUILDER_DIR%" --target strata-cluster-build
  if errorlevel 1 exit /b 1
)
"%STRATA_BUILDER_DIR%\strata-cluster-build.exe" --source-root "%STRATA_BUILDER_ROOT%" %*
exit /b %errorlevel%
