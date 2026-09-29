@echo off
rem ---------------------------------------------------------------------------
rem mchose-tray deploy script
rem
rem WHY THIS IS NEEDED:
rem   The DSH session workspace is tagged with a "Low Mandatory Level" integrity
rem   label. Any process started from there runs at Low integrity, and Windows
rem   UIPI then blocks the message that registers a tray icon, so
rem   Shell_NotifyIconW(NIM_ADD) fails with ERROR_ACCESS_DENIED (5) and the
rem   program shows no icon at all. Copying the exe to a normal folder makes it
rem   run at Medium integrity, same as explorer, and the icon works.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"
set "DEST=%LOCALAPPDATA%\Programs\mchose-tray"

if not exist bin\mchose-tray.exe (
    echo [ERROR] bin\mchose-tray.exe not found. Run build.bat first.
    exit /b 1
)

echo [1/3] Installing to "%DEST%"
if not exist "%DEST%" mkdir "%DEST%"
copy /y bin\mchose-tray.exe "%DEST%\mchose-tray.exe" >nul
if errorlevel 1 (
    echo [ERROR] Copy failed. Is an old instance still running?
    echo         Close it from the tray menu and retry.
    exit /b 1
)

echo [2/3] Clearing any inherited Low integrity label
icacls "%DEST%\mchose-tray.exe" /setintegritylevel Medium >nul 2>nul

echo [3/3] Starting
start "" "%DEST%\mchose-tray.exe"

echo Done. Installed at: %DEST%\mchose-tray.exe
echo If the icon is not visible in the taskbar corner, click the "^" overflow
echo arrow and drag it onto the taskbar - Windows 11 hides new icons by default.
endlocal
