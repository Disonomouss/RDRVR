@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RDRVR_uninstall.ps1" -FromCmd %*
echo %* | findstr /i /c:"-Quiet" >nul || (echo. & pause)
rem Uninstalled: this file goes too (deleted only after cmd has stopped reading it)
if not exist "%~dp0RDRVR_install.json" (goto) 2>nul & del "%~f0"
