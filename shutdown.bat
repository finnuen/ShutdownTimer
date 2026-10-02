@echo off
title Windows Shutdown Timer

echo ====================================
echo        SCHEDULE PC SHUTDOWN
echo ====================================
echo.
set /p "minutes=Enter minutes until shutdown: "

:: Convert minutes to seconds
set /a "seconds=minutes * 60"

:: Schedule the shutdown
shutdown.exe -s -f -t %seconds%

echo.
echo Computer will shut down in %minutes% minute(s) (%seconds% seconds).
echo Press any key to close this window (timer will keep running).
echo.
echo NOTE: To cancel, run "shutdown -a" or make a cancel shortcut.
echo ====================================
pause >nul