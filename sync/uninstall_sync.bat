@echo off
:: Removes the Ardosia Sync startup shortcut.

set "SHORTCUT=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Ardosia Sync.lnk"

if exist "%SHORTCUT%" (
    del "%SHORTCUT%"
    echo Ardosia Sync startup shortcut removed.
) else (
    echo No startup shortcut found — nothing to remove.
)
echo.
pause
