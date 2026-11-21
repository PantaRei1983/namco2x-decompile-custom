@echo off
rem  LAN TEST, on the MAIN PC only, once: makes the results folder and shares it on the network (needs administrator: Windows asks).
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0lan-test.ps1" -Share
echo.
pause
