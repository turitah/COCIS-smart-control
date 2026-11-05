@echo off
rem ---------------------------------------------------------------------------
rem  idf.py shim for this project.
rem  `idf.py ...` is not a global command on this PC because ESP-IDF is installed
rem  by the Espressif Installation Manager. This shim forwards the arguments to
rem  idf.ps1, which loads the correct ESP-IDF v6.1 environment and python venv.
rem
rem  Usage (from any folder):
rem      idf.py build
rem      idf.py -p COM5 flash monitor
rem ---------------------------------------------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf.ps1" %*
exit /b %ERRORLEVEL%
