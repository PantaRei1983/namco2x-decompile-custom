@echo off
rem  LAN TEST, on EVERY PC of the test (the main PC too): pick the game and this PC's cabinet number; the game starts full screen,
rem  and when it is closed its logs go to the results folder. The game's own link settings are put back afterwards.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0lan-test.ps1" -Run
echo.
pause
