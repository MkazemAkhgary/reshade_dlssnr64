@echo off
setlocal EnableExtensions

REM ============================================================
REM  Incremental build / type-check for reshade_dlssnr.
REM
REM  Mirrors the "Fast iteration" preset of build_template.bat:
REM  Release, no Tracy, no trace, no shader verification, no
REM  profiling domains. Uses the project's PCH, so only changed
REM  translation units are recompiled - far faster than a
REM  from-scratch cl.exe sweep.
REM
REM  Usage:  check_build.bat [Debug] [Rebuild]
REM ============================================================

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "SOLUTION_PATH=%SCRIPT_DIR%\reshade_dlssnr\reshade_dlssnr.sln"

set "CONFIG=Release"
set "BUILD_TARGET=Build"
for %%A in (%*) do (
    if /i "%%~A"=="Debug" set "CONFIG=Debug"
    if /i "%%~A"=="Rebuild" set "BUILD_TARGET=Rebuild"
)

if not exist "%SOLUTION_PATH%" (
    echo ERROR: solution not found: "%SOLUTION_PATH%"
    exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found.
    exit /b 1
)

set "MSBUILD_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\amd64\MSBuild.exe 2^>nul`) do (
    if not defined MSBUILD_PATH set "MSBUILD_PATH=%%i"
)
if not defined MSBUILD_PATH (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe 2^>nul`) do (
        if not defined MSBUILD_PATH set "MSBUILD_PATH=%%i"
    )
)
if not defined MSBUILD_PATH (
    echo ERROR: MSBuild.exe not found.
    exit /b 1
)

echo Building %CONFIG%^|x64 (%BUILD_TARGET%)...

REM MSBuild's tool task builds a case-sensitive environment dictionary, so an
REM inherited HTTPS_PROXY/https_proxy pair trips MSB6001 before CL.exe starts.
set "https_proxy="
set "http_proxy="
set "all_proxy="
set "no_proxy="

"%MSBUILD_PATH%" "%SOLUTION_PATH%" /t:%BUILD_TARGET% /m /v:minimal /nologo ^
 /p:Configuration=%CONFIG% /p:Platform=x64 ^
 /p:DlssnrTracyEnabled=false ^
 /p:DlssnrTraceEnabled=false ^
 /p:DlssnrMutexCompatibility=true ^
 /p:VerifyShaders=false ^
 /p:PrecompileShaders=false ^
 /p:DlssnrProfileVerbosity=0 ^
 /p:DlssnrProfileDomainCore=0 ^
 /p:DlssnrProfileDomainRender=0 ^
 /p:DlssnrProfileDomainResource=0 ^
 /p:DlssnrProfileDomainAsync=0 ^
 /p:DlssnrProfileDomainUi=0 ^
 /p:DlssnrProfileDomainCtl=0 ^
 /p:DlssnrProfileDomainGpu=0

if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)
echo BUILD OK
exit /b 0
