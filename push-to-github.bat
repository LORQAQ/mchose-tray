@echo off
rem ---------------------------------------------------------------------------
rem  Push mchose-tray to GitHub
rem
rem  Situation this script handles:
rem    The local history (3 commits) has NO common ancestor with the remote
rem    "Initial commit" that GitHub created when the repository was made.
rem    That remote commit contains nothing but an auto-generated README stub,
rem    so overwriting it is safe - the script asks before doing so.
rem
rem  This script never stores credentials. Git Credential Manager prompts
rem  in the browser once, then remembers the sign-in.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"
set "REPO=mchose-tray"

echo ============================================================
echo   Upload %REPO% to GitHub
echo ============================================================
echo.

set "GHUSER=LORQAQ"
set "INPUT="
set /p INPUT=GitHub username [press Enter for %GHUSER%]: 
if not "%INPUT%"=="" set "GHUSER=%INPUT%"

set "URL=https://github.com/%GHUSER%/%REPO%.git"
echo.
echo   Target: %URL%
echo.

git rev-parse --git-dir >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Not a git repository. Run this from the %REPO% folder.
    pause
    exit /b 1
)

git remote get-url origin >nul 2>nul
if errorlevel 1 (
    git remote add origin "%URL%"
    echo [1/3] Remote "origin" added.
) else (
    git remote set-url origin "%URL%"
    echo [1/3] Remote "origin" set.
)

echo [2/3] Fetching remote state...
git fetch --prune origin >nul 2>nul

echo [3/3] Pushing...
echo       A browser window may open for GitHub sign-in the first time.
echo.
git push -u origin main
if not errorlevel 1 goto pushed

echo.
echo ------------------------------------------------------------
echo   Normal push was rejected.
echo.
echo   The remote already has a commit sharing no history with this
echo   repository - the auto-generated "Initial commit" (README stub)
echo   created when the repo was made on GitHub.
echo.
echo   Overwriting it is safe: nothing but that stub is lost.
echo   Everything of value is in the local commits below.
echo ------------------------------------------------------------
git log --oneline -3
echo.
set "ANS="
set /p ANS=Overwrite the remote with the local history? [y/N]: 
if /i not "%ANS%"=="y" (
    echo.
    echo Aborted - nothing was pushed.
    echo.
    echo Manual alternative if you prefer merging instead:
    echo     git pull --no-rebase --allow-unrelated-histories origin main
    echo     git push -u origin main
    pause
    exit /b 1
)

echo.
echo Force pushing...
git push --force -u origin main
if errorlevel 1 (
    echo.
    echo [ERROR] Push still failed. Check:
    echo         - repository name / username correct?
    echo         - sign-in completed in the browser?
    pause
    exit /b 1
)

:pushed
echo.
echo ============================================================
echo   Done!  https://github.com/%GHUSER%/%REPO%
echo ============================================================
git log --oneline -3
echo.
pause
endlocal
