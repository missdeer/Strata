@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\rebase-verify-env.log 2>&1
if errorlevel 1 exit /b 1
set "PATH=D:\Projects\Strata\.rocm-win\Lib\site-packages\_rocm_sdk_devel\bin;%PATH%"
python build/rebase-verify-windows.py %*
exit /b %errorlevel%
