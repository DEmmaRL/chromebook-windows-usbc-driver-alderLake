@echo off
rem install.cmd - run install.ps1 elevated, regardless of the PowerShell execution policy.
setlocal
net session >nul 2>&1
if not errorlevel 1 goto :run

rem Not elevated: relaunch elevated in a window that stays open.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -FilePath 'cmd.exe' -ArgumentList '/k','\"%~f0\" %*'"
exit /b

:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
