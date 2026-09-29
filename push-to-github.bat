@echo off
rem ---------------------------------------------------------------------------
rem  Push mchose-tray to GitHub
rem
rem  Before running:
rem    1) Create an EMPTY repo named "mchose-tray" at https://github.com/new
rem       Do NOT tick "Add a README", .gitignore or license.
rem    2) Know your GitHub username.
rem
rem  This script only adds the "origin" remote and pushes.
rem  It does not store credentials itself - Git Credential Manager prompts once.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

echo ============================================================
echo   Upload mchose-tray to GitHub
echo ============================================================
echo.
echo   Prerequisite: an EMPTY repo named "mchose-tray" must already
echo   exist on your account. Create it at: https://github.com/new
echo   (do NOT tick Add README / .gitignore / license)
echo.

set "GHUSER="
set /p GHUSER=Your GitHub username: 
if "%GHUSER%"=="" (
    echo [ERROR] Username is empty.
    pause
    exit /b 1
)

set "URL=https://github.com/%GHUSER%/mchose-tray.git"
echo.
echo   Target: %URL%
echo.

git rev-parse --git-dir >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Not a git repository. Run this from the mchose-tray folder.
    pause
    exit /b 1
)

git remote get-url origin >nul 2>nul
if errorlevel 1 (
    git remote add origin "%URL%"
    echo [1/2] Remote "origin" added.
) else (
    git remote set-url origin "%URL%"
    echo [1/2] Remote "origin" updated.
)

echo [2/2] Pushing to GitHub...
echo       A browser window may open for sign-in the first time.
echo.
git push -u origin main
if errorlevel 1 (
    echo.
    echo [ERROR] Push failed. Most likely causes:
    echo         - The repository does not exist yet on GitHub
    echo           ^(create it at https://github.com/new, name: mchose-tray^)
    echo         - The username is wrong
    echo         - Sign-in was cancelled
    echo.
    echo   Manual alternative:
    echo         git remote set-url origin %URL%
    echo         git push -u origin main
    pause
    exit /b 1
)

echo.
echo ============================================================
echo   Done!  https://github.com/%GHUSER%/mchose-tray
echo ============================================================
pause
endlocal
