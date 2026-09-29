@echo off
rem ---------------------------------------------------------------------------
rem mchose-tray build script (MinGW-w64 / GCC)
rem   [1/3] main program    -> bin\mchose-tray.exe
rem   [2/3] protocol tools   -> tools\*.exe
rem   [3/3] summary
rem
rem NOTE: do NOT run the built exe from a directory carrying a
rem       "Low Mandatory Level" integrity label - the process would run at Low
rem       integrity and UIPI would block tray icon registration
rem       (Shell_NotifyIcon returns ERROR_ACCESS_DENIED). Use deploy.bat.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist bin mkdir bin
if not exist obj mkdir obj

rem --- version resource (optional; skipped if windres is unavailable) ---
set "RES="
where windres >nul 2>nul
if not errorlevel 1 (
    windres src\mchose-tray.rc -O coff -o obj\mchose-tray.res >nul 2>nul
    if not errorlevel 1 set "RES=obj\mchose-tray.res"
)

where g++ >nul 2>nul
if errorlevel 1 goto nogpp

echo [1/3] Building mchose-tray.exe ...
g++ -O2 -Wall -Wextra -municode -mwindows -static -s src\main.cpp src\mchose_protocol.cpp src\model_db.cpp src\device_manager.cpp src\tray_ui.cpp %RES% -o bin\mchose-tray.exe -lsetupapi -lhid -luser32 -lgdi32 -lshell32 -ladvapi32
if errorlevel 1 goto buildfail
if defined RES (echo      version resource: obj\mchose-tray.res) else (echo      version resource: SKIPPED ^(windres not found^))
for %%F in (bin\mchose-tray.exe) do echo      OK  %%~nxF  (%%~zF bytes)

echo [2/3] Building tools ...
call :tool hid_probe      "-lhid -lsetupapi"
call :tool hid_sniff      "-lhid -lsetupapi"
call :tool mchose_probe   "-lhid -lsetupapi"
call :tool mchose_monitor "-lhid -lsetupapi"
call :tool polling_rate   "-luser32"
call :tool query_il       "-ladvapi32 -luser32"
call :tool tray_test      "-municode -mwindows -lshell32 -luser32"
call :tool strings        ""

echo [3/3] Done.
echo.
echo Next: deploy.bat   (installs to %%LOCALAPPDATA%%\Programs\mchose-tray and starts it)
goto end

:tool
set "TNAME=%~1"
set "TLIBS=%~2"
if not exist "tools\%TNAME%.c" goto :eof
gcc -O2 -Wall -o "tools\%TNAME%.exe" "tools\%TNAME%.c" %TLIBS% 2>nul
if errorlevel 1 (echo      SKIP %TNAME%  ^(build failed^)) else (echo      OK   %TNAME%.exe)
goto :eof

:nogpp
echo [ERROR] g++ not found. Install MinGW-w64 and add it to PATH.
exit /b 1

:buildfail
echo [ERROR] Build failed.
exit /b 1

:end
endlocal
