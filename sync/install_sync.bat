@echo off
:: Creates a startup shortcut so Ardosia Sync runs on every login.
:: Double-click this file to install. Run uninstall_sync.bat to remove.

set "STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
set "SCRIPT=%~dp0ardosia_sync.py"

:: Create a VBS helper to make the shortcut (avoids PowerShell execution policy issues)
set "VBS=%TEMP%\ardosia_shortcut.vbs"
> "%VBS%" echo Set ws = CreateObject("WScript.Shell")
>>"%VBS%" echo Set s = ws.CreateShortcut("%STARTUP%\Ardosia Sync.lnk")
>>"%VBS%" echo s.TargetPath = "pythonw.exe"
>>"%VBS%" echo s.Arguments = """%SCRIPT%"""
>>"%VBS%" echo s.Save
cscript //nologo "%VBS%"
del "%VBS%"

:: Start it right now in the background (silent, no window)
start "" pythonw.exe "%SCRIPT%"

echo.
echo Ardosia Sync installed and running.
echo It will start automatically on every login.
echo.
pause
