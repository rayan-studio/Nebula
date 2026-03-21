@echo off
setlocal EnableExtensions EnableDelayedExpansion

echo [Nebula] Installing build dependencies...

echo.
set "WINGET_CMD="
for %%P in (winget.exe) do where %%P >nul 2>&1 && set "WINGET_CMD=winget.exe"
if not defined WINGET_CMD (
  set "WINGET_LOCAL=%LOCALAPPDATA%\Microsoft\WindowsApps\winget.exe"
  if exist "%WINGET_LOCAL%" set "WINGET_CMD=%WINGET_LOCAL%"
)
if not defined WINGET_CMD (
  echo [Error] winget not found. Install App Installer from the Microsoft Store first.
  exit /b 1
)

call :install_package "Git.Git" "Git"
call :install_package "Python.Python.3.11" "Python 3.11"
call :install_package "Kitware.CMake" "CMake"
call :install_package "LLVM.LLVM" "LLVM (clangd)"
call :install_package "MSYS2.MSYS2" "MSYS2"

echo.
call :install_msys2_toolchain

echo.
call :clone_external

echo.
call :install_python_requirements

echo.
echo [Nebula] Done.
echo If gcc is not on PATH, add MSYS2's mingw64 bin folder to PATH:
echo   %PROGRAMFILES%\MSYS2\mingw64\bin
exit /b 0

:install_package
set "PKG_ID=%~1"
set "PKG_NAME=%~2"

echo [Nebula] Checking %PKG_NAME%...
%WINGET_CMD% list --id %PKG_ID% >nul 2>&1
if %errorlevel%==0 (
  echo [Nebula] %PKG_NAME% already installed.
  goto :eof
)

echo [Nebula] Installing %PKG_NAME%...
%WINGET_CMD% install --id %PKG_ID% --exact --accept-source-agreements --accept-package-agreements
if not %errorlevel%==0 (
  echo [Error] Failed to install %PKG_NAME%.
  exit /b 1
)

goto :eof

:install_msys2_toolchain
set "MSYS2_DIR=%PROGRAMFILES%\MSYS2"
if exist "%ProgramFiles(x86)%\MSYS2" set "MSYS2_DIR=%ProgramFiles(x86)%\MSYS2"

if not exist "%MSYS2_DIR%\usr\bin\bash.exe" (
  echo [Error] MSYS2 not found at %MSYS2_DIR%.
  echo Install MSYS2 or adjust the path in this script.
  exit /b 1
)

echo [Nebula] Installing MinGW GCC toolchain via MSYS2...
"%MSYS2_DIR%\usr\bin\bash.exe" -lc "pacman -S --noconfirm --needed mingw-w64-x86_64-toolchain"
if not %errorlevel%==0 (
  echo [Error] Failed to install MinGW toolchain.
  exit /b 1
)

goto :eof

:clone_external
set "ROOT_DIR=%~dp0.."
set "EXTERNAL_DIR=%ROOT_DIR%\external"

if not exist "%EXTERNAL_DIR%" (
  mkdir "%EXTERNAL_DIR%"
)

echo [Nebula] Cloning external dependencies...

call :clone_repo "libvterm" "https://github.com/neovim/libvterm.git" "%EXTERNAL_DIR%\libvterm" "%EXTERNAL_DIR%\libvterm\include\vterm.h"
if not %errorlevel%==0 exit /b 1

call :clone_repo "ggwave" "https://github.com/ggerganov/ggwave.git" "%EXTERNAL_DIR%\ggwave" "%EXTERNAL_DIR%\ggwave\include\ggwave\ggwave.h"
if not %errorlevel%==0 exit /b 1

call :clone_repo "nanosvg" "https://github.com/memononen/nanosvg.git" "%EXTERNAL_DIR%\nanosvg" "%EXTERNAL_DIR%\nanosvg\src\nanosvg.h"
if not %errorlevel%==0 exit /b 1

call :clone_repo "libgit2" "https://github.com/libgit2/libgit2.git" "%EXTERNAL_DIR%\libgit2" "%EXTERNAL_DIR%\libgit2\include\git2.h"
if not %errorlevel%==0 exit /b 1

goto :eof

:clone_repo
set "DEP_NAME=%~1"
set "DEP_URL=%~2"
set "DEP_DIR=%~3"
set "DEP_MARKER=%~4"

if exist "%DEP_MARKER%" (
  echo [Nebula] %DEP_NAME% already exists.
  goto :eof
)

if exist "%DEP_DIR%" (
  dir /b "%DEP_DIR%" >nul 2>&1
  if not errorlevel 1 (
    echo [Error] %DEP_NAME% is present but incomplete at %DEP_DIR%.
    echo Remove the folder and rerun the script.
    exit /b 1
  )
) else (
  mkdir "%DEP_DIR%" >nul 2>&1
)

echo [Nebula] Cloning %DEP_NAME%...
git clone "%DEP_URL%" "%DEP_DIR%"
if not %errorlevel%==0 (
  echo [Error] Failed to clone %DEP_NAME%.
  exit /b 1
)

if not exist "%DEP_MARKER%" (
  echo [Error] %DEP_NAME% was cloned but expected files are missing.
  exit /b 1
)

goto :eof

:install_python_requirements
set "ROOT_DIR=%~dp0.."
set "REQ_FILE=%ROOT_DIR%\scripts\requirements.txt"

if not exist "%REQ_FILE%" (
  echo [Nebula] No Python requirements file found at %REQ_FILE%.
  goto :eof
)

echo [Nebula] Installing Python requirements...
python -m pip --version >nul 2>&1
if not %errorlevel%==0 (
  echo [Error] pip not found. Ensure Python is installed and pip is available.
  exit /b 1
)

python -m pip install --upgrade pip
if not %errorlevel%==0 (
  echo [Error] Failed to update pip.
  exit /b 1
)

python -m pip install -r "%REQ_FILE%"
if not %errorlevel%==0 (
  echo [Error] Failed to install Python requirements.
  exit /b 1
)

goto :eof
