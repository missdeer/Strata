@echo off
setlocal
python "%~dp0build_cluster.py" %*
exit /b %errorlevel%
