@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > tmp\consolidated-hip-package-env.log 2>&1
if errorlevel 1 exit /b 1
.rocm-win\Scripts\python.exe tools\hip\package_windows.py --build build-cluster/host17-core --rocm .rocm-win/Lib/site-packages/_rocm_sdk_devel --archs "gfx1102;gfx1103" --rocm-version 10.2.0a20260930 --out tmp/consolidated-hip-package --network --native-host > tmp\consolidated-hip-package.log 2>&1
set "result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content tmp/consolidated-hip-package.log -Tail 12"
exit /b %result%
