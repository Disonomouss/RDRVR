@echo off
rem The window stays open at the end (-Quiet: it does not), whatever the script did: an error is readable
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -FromCmd %*
set RDRVR_RC=%errorlevel%
echo %* | findstr /i /c:"-Quiet" >nul || (echo. & pause)
exit /b %RDRVR_RC%
