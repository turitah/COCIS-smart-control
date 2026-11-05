@echo off
rem Double-click to build the firmware, or run "build.cmd flash" from a terminal.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf.ps1" %*
set RESULT=%ERRORLEVEL%
if "%~1"=="" pause
exit /b %RESULT%
