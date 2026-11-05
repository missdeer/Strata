@echo off
setlocal
set "STRATA_ROCM_COMPILER_ROOT=D:\Projects\Strata\.rocm-win\Lib\site-packages\_rocm_sdk_core"
python tools/build_cluster.py --hosts host17 --build-dir host17=build-cluster/host17-core --no-install-rocm --jobs 6
exit /b %errorlevel%
