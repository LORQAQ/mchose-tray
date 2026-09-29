@echo off
rem ---------------------------------------------------------------------------
rem mchose-tray build script (MinGW-w64 / GCC)
rem Output: bin\mchose-tray.exe (statically linked, single file)
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist bin mkdir bin

where g++ >nul 2>nul
if errorlevel 1 goto nogpp

echo [1/2] Building mchose-tray.exe ...
g++ -O2 -Wall -municode -mwindows -static -s src\main.cpp src\mchose_protocol.cpp src\device_manager.cpp src\tray_ui.cpp -o bin\mchose-tray.exe -lsetupapi -lhid -luser32 -lgdi32 -lshell32 -ladvapi32
if errorlevel 1 goto buildfail

echo [2/2] Done.
for %%F in (bin\mchose-tray.exe) do echo      Output: %%~fF  (%%~zF bytes)
echo.
echo Optional: build the protocol tools in tools\
echo     gcc -O2 -o tools\mchose_probe.exe tools\mchose_probe.c -lhid -lsetupapi
echo     gcc -O2 -o tools\mchose_monitor.exe tools\mchose_monitor.c -lhid -lsetupapi
echo     gcc -O2 -o tools\hid_probe.exe tools\hid_probe.c -lhid -lsetupapi
goto end

:nogpp
echo [ERROR] g++ not found. Install MinGW-w64 and add it to PATH.
exit /b 1

:buildfail
echo [ERROR] Build failed.
exit /b 1

:end
endlocal
