@echo off
setlocal
cd /d "%~dp0"

set "CMAKE_EXE="
set "VSINSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

where cmake >nul 2>nul
if not errorlevel 1 set "CMAKE_EXE=cmake"

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
)

if not defined CMAKE_EXE (
    if defined VSINSTALL (
        if exist "%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "CMAKE_EXE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        )
    )
)

if not defined CMAKE_EXE (
    echo 未找到 cmake。请安装 CMake，或安装带 C++ 工具的 Visual Studio。
    exit /b 1
)

if defined VSINSTALL (
    if exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" (
        call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
        if errorlevel 1 exit /b 1
    )
)

"%CMAKE_EXE%" -S . -B build -DNOTION_CLIPBOARD_WIN_GUI=ON
if errorlevel 1 exit /b 1

"%CMAKE_EXE%" --build build --config Release
if errorlevel 1 exit /b 1

echo.
echo 已构建: %cd%\build\Release\notion_clipboard_win.exe
