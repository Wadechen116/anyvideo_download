@echo off
rem Build the Qt version: CMake + windeployqt.
rem Output: build\Release\AnyVideoDownload.exe (Qt DLLs deployed next to it)
rem Override the Qt kit with:  set QTDIR=C:\Qt\6.x.x\msvc2019_64
rem Visual Studio 2022 is preferred. When it is not installed (e.g. only a newer VS is),
rem falls back to the installed VS with its v143 (14.4x) toolset - Qt 6.5 headers do not compile
rem with MSVC 14.5x - + its bundled Ninja; output is still build\Release.
setlocal
cd /d "%~dp0"
if "%QTDIR%"=="" set "QTDIR=C:\Qt\6.5.1\msvc2019_64"
if not exist "%QTDIR%\bin\windeployqt.exe" (
    echo Qt kit not found: %QTDIR%
    exit /b 1
)

set "VS22="
for /d %%i in ("%ProgramFiles%\Microsoft Visual Studio\2022\*") do if exist "%%i\VC\Auxiliary" set "VS22=%%i"
if defined VS22 goto :vs2022

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSLATEST="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSLATEST=%%i"
if not defined VSLATEST (
    echo Visual Studio not found
    exit /b 1
)
call "%VSLATEST%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.4 >nul || exit /b 1
set "PATH=%VSLATEST%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QTDIR%" ^
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="%~dp0build\Release" || exit /b 1
cmake --build build-ninja || exit /b 1
goto :deploy

:vs2022
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="%QTDIR%" || exit /b 1
cmake --build build --config Release || exit /b 1

:deploy
"%QTDIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw ^
    build\Release\AnyVideoDownload.exe || exit /b 1
echo.
echo Built: %~dp0build\Release\AnyVideoDownload.exe
