@echo off
rem  LAN TEST, on the MAIN PC: a short report of every test in the results folder (linked or not, and any warnings).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0lan-test.ps1" -Check
echo.
pause
