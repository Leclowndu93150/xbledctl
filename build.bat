@echo off
setlocal

REM --- Find Visual Studio ---
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not defined VSINSTALLDIR (
    if exist "%VCVARS%" (
        echo Setting up MSVC environment...
        call "%VCVARS%" x64 >nul 2>&1
    ) else (
        echo ERROR: Visual Studio 2022 not found at expected location.
        echo Install VS2022 with C++ Desktop workload, or run from a Developer Command Prompt.
        exit /b 1
    )
)

REM --- Build ---
if not exist build mkdir build
cd build

"%CMAKE%" .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo.
    echo CMAKE configuration failed.
    exit /b 1
)

nmake
if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

cd ..
echo.
echo ========================================
echo   Build successful: build\xbledctl.exe
echo ========================================
