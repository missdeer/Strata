@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\rebase-20261012\report-env.log 2>&1
if errorlevel 1 exit /b 1
python build\rebase-final-report.py
exit /b %errorlevel%
