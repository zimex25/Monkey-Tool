@echo off
title Monkey Tool 1.2.2 - build
cd /d "%~dp0"

echo.
echo   Monkey Tool 1.2.2
echo   ================
echo.
echo   Building. If no compiler is installed, a portable one is
echo   downloaded automatically (about 90 MB, one time only).
echo.
echo   Do NOT click inside this window while it builds - Windows pauses
echo   whatever is running when you select text in a console, and the
echo   build then looks frozen. If that happens, press Esc.
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0BUILD.ps1"

if errorlevel 1 (
    echo.
    echo   The build did not finish. The messages above say why.
    echo.
)

echo.
echo   This window stays open so you can read any messages.
pause
