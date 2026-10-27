@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM ============================================================
REM  TEMPLATE - EDIT THIS LINE BEFORE RUNNING.
REM
REM  Replace __REPLACE_WITH_FULL_PATH_TO_SLN__ with the full path
REM  to the solution file you want to build, for example:
REM
REM      set "SOLUTION_PATH=C:\path\to\your\project.sln"
REM
REM  The output addon and the launch target are resolved relative
REM  to this script's own directory, so the script can be dropped
REM  into a game/build directory and pointed at any checkout.
REM ============================================================
set "SOLUTION_PATH=__REPLACE_WITH_FULL_PATH_TO_SLN__"

if "%SOLUTION_PATH%"=="__REPLACE_WITH_FULL_PATH_TO_SLN__" (
    echo ============================================================
    echo   SOLUTION_PATH has not been configured yet.
    echo ============================================================
    echo.
    echo This script is a template. Open it in a text editor and replace
    echo.
    echo     __REPLACE_WITH_FULL_PATH_TO_SLN__
    echo.
    echo with the full path to the solution file you want to build, e.g.
    echo.
    echo     set "SOLUTION_PATH=C:\path\to\your\project.sln"
    echo.
    echo The line to edit is near the top of this file.
    echo.
    pause
    exit /b 1
)
if not exist "%SOLUTION_PATH%" (
    echo ERROR: The configured solution file does not exist:
    echo   "%SOLUTION_PATH%"
    echo.
    echo Edit SOLUTION_PATH near the top of this script and point it at
    echo a valid .sln file.
    echo.
    pause
    exit /b 1
)

REM ============================================================
REM  Script-local output paths.
REM ============================================================
set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "ADDON_OUTPUT=%SCRIPT_DIR%\reshade_dlssnr64.addon64"

REM ============================================================
REM  Locate MSBuild via vswhere. Prefer the amd64 build: Visual
REM  Studio is 64-bit and uses the amd64 MSBuild, so matching that
REM  keeps VS builds and this script building the same way. Fall
REM  back to the generic path for installations that only expose
REM  Bin\MSBuild.exe.
REM ============================================================
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio with the C++ workload.
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
    echo ERROR: MSBuild.exe not found. Install Visual Studio with the C++ workload.
    exit /b 1
)

REM ============================================================
REM  Defaults. These match the project's own defaults.
REM ============================================================
call :SetDefaults

REM ============================================================
REM  Last used configuration, read from the last line of this
REM  file. :Persist writes it back there.
REM ============================================================
set "LAST_LINE="
for /f "tokens=2 delims==" %%L in ('findstr /b /c:"REM CFG=" "%~f0" 2^>nul') do set "LAST_LINE=%%L"
set "LAST_TARGET="
for /f "tokens=1* delims==" %%a in ('findstr /b /c:"REM TARGET_EXE=" "%~f0" 2^>nul') do set "LAST_TARGET=%%b"

set "HAS_LAST="
set "LAST_SUMMARY=none yet"
if defined LAST_LINE (
    for /f "tokens=1-15" %%a in ("!LAST_LINE!") do (
        set "LAST_BUILD_TARGET=%%a"
        set "LAST_CONFIG=%%b"
        set "LAST_TRACY=%%c"
        set "LAST_VERBOSITY=%%d"
        set "LAST_D_CORE=%%e"
        set "LAST_D_RENDER=%%f"
        set "LAST_D_RESOURCE=%%g"
        set "LAST_D_ASYNC=%%h"
        set "LAST_D_UI=%%i"
        set "LAST_D_CTL=%%j"
        set "LAST_D_GPU=%%k"
        set "LAST_TRACE=%%l"
        set "LAST_MUTEX=%%m"
        set "LAST_VERIFY=%%n"
        set "LAST_PRECOMPILE=%%o"
        set "LAST_SUMMARY=%%a %%b, tracy=%%c verb=%%d trace=%%l"
        set "HAS_LAST=1"
    )
)

REM A remembered target that no longer exists has to be re-picked.
set "TARGET_MISSING="
if defined LAST_TARGET if not exist "%SCRIPT_DIR%\!LAST_TARGET!" (
    set "TARGET_MISSING=1"
    set "LAST_TARGET="
)

REM ============================================================
REM  Executables next to this script, alphabetical.
REM ============================================================
set "EXE_COUNT=0"
for /f "delims=" %%F in ('dir /b /a-d /o:n "%SCRIPT_DIR%\*.exe" 2^>nul') do (
    set /a EXE_COUNT+=1
    set "EXE_!EXE_COUNT!=%%F"
)

REM ============================================================
REM  Configuration menu. Presets set the switches; the launch
REM  target is remembered separately and survives every preset.
REM ============================================================
:MENU
cls
echo ============================================================
echo   DLSSNR build
echo ============================================================
echo.
if defined MENU_WARNING (
    echo   !MENU_WARNING!
    echo.
    set "MENU_WARNING="
)
if defined LAST_TARGET (
    echo Launch target: !LAST_TARGET!
) else if defined TARGET_MISSING (
    echo Launch target: the remembered executable is gone
) else (
    echo Launch target: not chosen yet
)
echo.
echo   1. Default          Release, no Tracy/trace, verify shaders  [default]
echo   2. Last used        !LAST_SUMMARY!
echo   3. Fast iteration   Release, no Tracy/trace/verify, no domains
echo   4. Profile deep     Release, Tracy verbosity 3, GPU zones on
echo   5. Debug            Debug, Tracy, trace, verify shaders
echo   6. Debug fast       Debug, no Tracy/trace/verify
echo   7. Shipped          Release, no Tracy/trace, precompiled shaders
echo   8. Custom           ask every switch
echo.
set "MENU_CHOICE="
set /p "MENU_CHOICE=Choose [1-8]: "
if not defined MENU_CHOICE set "MENU_CHOICE=1"

if "%MENU_CHOICE%"=="1" goto :PRESET_DEFAULT
if "%MENU_CHOICE%"=="2" goto :PRESET_LAST
if "%MENU_CHOICE%"=="3" goto :PRESET_FAST
if "%MENU_CHOICE%"=="4" goto :PRESET_PROFILE
if "%MENU_CHOICE%"=="5" goto :PRESET_DEBUG
if "%MENU_CHOICE%"=="6" goto :PRESET_DEBUGFAST
if "%MENU_CHOICE%"=="7" goto :PRESET_SHIPPED
if "%MENU_CHOICE%"=="8" goto :CUSTOM_CONFIG
set "MENU_WARNING=Invalid choice - please enter 1 through 8."
goto :MENU

:PRESET_DEFAULT
set "TARGET_EXE=%LAST_TARGET%"
goto :AFTER_PRESET

:PRESET_LAST
if not defined HAS_LAST (
    echo.
    echo No saved configuration yet; using the default preset.
    timeout /t 2 >nul
    set "TARGET_EXE=%LAST_TARGET%"
    goto :AFTER_PRESET
)
set "BUILD_TARGET=!LAST_BUILD_TARGET!"
set "CONFIG=!LAST_CONFIG!"
set "TRACY_ENABLED=!LAST_TRACY!"
set "PROFILE_VERBOSITY=!LAST_VERBOSITY!"
set "DOMAIN_CORE=!LAST_D_CORE!"
set "DOMAIN_RENDER=!LAST_D_RENDER!"
set "DOMAIN_RESOURCE=!LAST_D_RESOURCE!"
set "DOMAIN_ASYNC=!LAST_D_ASYNC!"
set "DOMAIN_UI=!LAST_D_UI!"
set "DOMAIN_CTL=!LAST_D_CTL!"
set "DOMAIN_GPU=!LAST_D_GPU!"
set "TRACE_ENABLED=!LAST_TRACE!"
set "MUTEX_COMPAT=!LAST_MUTEX!"
set "VERIFY_SHADERS=!LAST_VERIFY!"
set "PRECOMPILE_SHADERS=!LAST_PRECOMPILE!"
set "TARGET_EXE=!LAST_TARGET!"
goto :AFTER_PRESET

:PRESET_FAST
set "TRACY_ENABLED=false"
set "PROFILE_VERBOSITY=0"
set "TRACE_ENABLED=false"
set "VERIFY_SHADERS=false"
set "PRECOMPILE_SHADERS=false"
set "TARGET_EXE=%LAST_TARGET%"
goto :AFTER_PRESET

:PRESET_PROFILE
set "TRACY_ENABLED=true"
set "PROFILE_VERBOSITY=3"
set "DOMAIN_GPU=1"
set "TRACE_ENABLED=true"
set "VERIFY_SHADERS=false"
set "TARGET_EXE=%LAST_TARGET%"
goto :AFTER_PRESET

:PRESET_DEBUG
set "CONFIG=Debug"
set "TARGET_EXE=%LAST_TARGET%"
goto :AFTER_PRESET

:PRESET_DEBUGFAST
set "CONFIG=Debug"
set "TRACY_ENABLED=false"
set "PROFILE_VERBOSITY=0"
set "TRACE_ENABLED=false"
set "VERIFY_SHADERS=false"
set "PRECOMPILE_SHADERS=false"
set "TARGET_EXE=%LAST_TARGET%"
goto :AFTER_PRESET

:PRESET_SHIPPED
set "TRACY_ENABLED=false"
set "PROFILE_VERBOSITY=0"
set "TRACE_ENABLED=false"
set "VERIFY_SHADERS=true"
set "PRECOMPILE_SHADERS=true"
set "TARGET_EXE=%LAST_TARGET%"
goto :AFTER_PRESET

:AFTER_PRESET
REM No instrumentation without Tracy, so the dependent switches follow.
if "%TRACY_ENABLED%"=="false" call :DisableProfiling
if not defined TARGET_EXE call :SELECT_EXE
goto :SUMMARY

:DisableProfiling
set "PROFILE_VERBOSITY=0"
set "DOMAIN_CORE=0"
set "DOMAIN_RENDER=0"
set "DOMAIN_RESOURCE=0"
set "DOMAIN_ASYNC=0"
set "DOMAIN_UI=0"
set "DOMAIN_CTL=0"
set "DOMAIN_GPU=0"
exit /b 0

:CUSTOM_CONFIG

:ASK_BUILD_TARGET
echo.
echo Build mode:
echo   1. Build   (incremental, reuses unchanged objects)  [default]
echo   2. Rebuild (Clean + Build, full from scratch)
set "BUILD_TARGET_CHOICE="
set /p "BUILD_TARGET_CHOICE=Choose [1-2]: "
if not defined BUILD_TARGET_CHOICE set "BUILD_TARGET_CHOICE=1"
if "%BUILD_TARGET_CHOICE%"=="1" (
    set "BUILD_TARGET=Build"
    goto :ASK_CONFIG
)
if "%BUILD_TARGET_CHOICE%"=="2" (
    set "BUILD_TARGET=Rebuild"
    goto :ASK_CONFIG
)
echo Invalid choice.
goto :ASK_BUILD_TARGET

:ASK_CONFIG
echo.
echo Configuration:
echo   1. Release  [default]
echo   2. Debug
set "CONFIG_CHOICE="
set /p "CONFIG_CHOICE=Choose [1-2]: "
if not defined CONFIG_CHOICE set "CONFIG_CHOICE=1"
if "%CONFIG_CHOICE%"=="1" goto :ASK_TRACY
if "%CONFIG_CHOICE%"=="2" (
    set "CONFIG=Debug"
    goto :ASK_TRACY
)
echo Invalid choice.
goto :ASK_CONFIG

:ASK_TRACY
echo.
set "TRACY_CHOICE="
set /p "TRACY_CHOICE=Enable Tracy profiling (links TracyClient)? [y/N]: "
if not defined TRACY_CHOICE set "TRACY_CHOICE=N"
if /i "%TRACY_CHOICE:~0,1%"=="n" (
    set "TRACY_ENABLED=false"
) else (
    set "TRACY_ENABLED=true"
)
REM Nothing to instrument without Tracy; skip the profiling questions.
if "%TRACY_ENABLED%"=="false" (
    call :DisableProfiling
    goto :ASK_TRACE
)

:ASK_VERBOSITY
echo.
echo Profile verbosity:
echo   0 = no instrumentation
echo   1 = lifecycle/frame   [default]
echo   2 = pass/resource/async
echo   3 = deep helpers
set "VERBOSITY_CHOICE="
set /p "VERBOSITY_CHOICE=Choose [0-3]: "
if not defined VERBOSITY_CHOICE set "VERBOSITY_CHOICE=1"
if "%VERBOSITY_CHOICE%"=="0" goto :ASK_DOMAINS
if "%VERBOSITY_CHOICE%"=="1" goto :ASK_DOMAINS
if "%VERBOSITY_CHOICE%"=="2" goto :ASK_DOMAINS
if "%VERBOSITY_CHOICE%"=="3" goto :ASK_DOMAINS
echo Invalid choice.
goto :ASK_VERBOSITY

:ASK_DOMAINS
set "PROFILE_VERBOSITY=%VERBOSITY_CHOICE%"
if "%PROFILE_VERBOSITY%"=="0" (
    call :DisableProfiling
    goto :ASK_TRACE
)

echo.
echo Profile domains gate the zones of each subsystem. GPU zones also need
echo verbosity 2 or higher to be recorded.
set "DOMAINS_ALL="
set /p "DOMAINS_ALL=Keep all domains at their defaults (Core/Render/Resource/Async/Ui/Ctl on, Gpu off)? [Y/n]: "
if not defined DOMAINS_ALL set "DOMAINS_ALL=Y"
if /i not "%DOMAINS_ALL:~0,1%"=="n" goto :ASK_TRACE

call :ASK_DOMAIN "Core"     DOMAIN_CORE     Y
call :ASK_DOMAIN "Render"   DOMAIN_RENDER   Y
call :ASK_DOMAIN "Resource" DOMAIN_RESOURCE Y
call :ASK_DOMAIN "Async"    DOMAIN_ASYNC    Y
call :ASK_DOMAIN "Ui"       DOMAIN_UI       Y
call :ASK_DOMAIN "Ctl"      DOMAIN_CTL      Y
call :ASK_DOMAIN "Gpu"      DOMAIN_GPU      N

:ASK_TRACE
echo.
echo The pipeline trace (TRACE_CMD/TRACE_EVT/TRACE_CFG) is still gated at
echo runtime by the DLSSNR_TRACE environment variable.
set "TRACE_CHOICE="
set /p "TRACE_CHOICE=Compile the pipeline trace in? [y/N]: "
if not defined TRACE_CHOICE set "TRACE_CHOICE=N"
if /i "%TRACE_CHOICE:~0,1%"=="n" (
    set "TRACE_ENABLED=false"
) else (
    set "TRACE_ENABLED=true"
)

:ASK_MUTEX
echo.
echo _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR keeps std::mutex compatible with a
echo game that ships an older msvcp140.dll. Only turn it off when every target
echo runtime is at least the toolset's own version.
set "MUTEX_CHOICE="
set /p "MUTEX_CHOICE=Keep the msvcp140 mutex workaround? [Y/n]: "
if not defined MUTEX_CHOICE set "MUTEX_CHOICE=Y"
if /i "%MUTEX_CHOICE:~0,1%"=="n" (
    set "MUTEX_COMPAT=false"
) else (
    set "MUTEX_COMPAT=true"
)

REM Precompile runs FXC over every shader, which subsumes the verify pass,
REM so the verify question only makes sense when precompiling is off.
:ASK_PRECOMPILE
echo.
set "PRECOMPILE_CHOICE="
set /p "PRECOMPILE_CHOICE=Precompile shaders (FXC to bytecode)? [y/N]: "
if not defined PRECOMPILE_CHOICE set "PRECOMPILE_CHOICE=N"
if /i "%PRECOMPILE_CHOICE:~0,1%"=="y" (
    set "PRECOMPILE_SHADERS=true"
    set "VERIFY_SHADERS=true"
    goto :ASK_TARGET
)
set "PRECOMPILE_SHADERS=false"

:ASK_VERIFY
echo.
set "VERIFY_CHOICE="
set /p "VERIFY_CHOICE=Verify shaders via FXC before C++ compile? [Y/n]: "
if not defined VERIFY_CHOICE set "VERIFY_CHOICE=Y"
if /i "%VERIFY_CHOICE:~0,1%"=="n" (
    set "VERIFY_SHADERS=false"
) else (
    set "VERIFY_SHADERS=true"
)

:ASK_TARGET
echo.
if defined TARGET_EXE (
    echo Current launch target: !TARGET_EXE!
) else (
    echo Current launch target: not chosen yet
)
set "CHANGE_TARGET="
set /p "CHANGE_TARGET=Change the launch target? [y/N]: "
if not defined CHANGE_TARGET set "CHANGE_TARGET=N"
if /i "%CHANGE_TARGET:~0,1%"=="y" (
    call :SELECT_EXE
    goto :SUMMARY
)
if not defined TARGET_EXE call :SELECT_EXE

:SUMMARY
echo.
echo ============================================================
echo   Build target      : %BUILD_TARGET%
echo   Configuration     : %CONFIG%
if "%TRACY_ENABLED%"=="true" (
    echo   Tracy             : true
    echo   Profile verbosity : %PROFILE_VERBOSITY%
    echo   Domains           : C=%DOMAIN_CORE% R=%DOMAIN_RENDER% S=%DOMAIN_RESOURCE% A=%DOMAIN_ASYNC% U=%DOMAIN_UI% T=%DOMAIN_CTL% G=%DOMAIN_GPU%
) else (
    echo   Tracy             : false - no instrumentation
)
echo   Pipeline trace    : %TRACE_ENABLED%
echo   Mutex workaround  : %MUTEX_COMPAT%
echo   Verify shaders    : %VERIFY_SHADERS%
echo   Precompile shaders: %PRECOMPILE_SHADERS%
echo   Launch target     : %TARGET_EXE%
echo   MSBuild           : %MSBUILD_PATH%
echo   Addon output      : %ADDON_OUTPUT%
echo ============================================================
echo.

REM ============================================================
REM  Build. A failure offers a plain retry, a full rebuild, or an
REM  abort -- a stale object is the usual reason a retry fails.
REM ============================================================
:BUILD
echo [1/3] Building (%BUILD_TARGET%)...
echo.

"%MSBUILD_PATH%" "%SOLUTION_PATH%" /t:%BUILD_TARGET% /p:Configuration=%CONFIG% /p:Platform=x64 /p:DlssnrTracyEnabled=%TRACY_ENABLED% /p:DlssnrProfileVerbosity=%PROFILE_VERBOSITY% /p:DlssnrProfileDomainCore=%DOMAIN_CORE% /p:DlssnrProfileDomainRender=%DOMAIN_RENDER% /p:DlssnrProfileDomainResource=%DOMAIN_RESOURCE% /p:DlssnrProfileDomainAsync=%DOMAIN_ASYNC% /p:DlssnrProfileDomainUi=%DOMAIN_UI% /p:DlssnrProfileDomainCtl=%DOMAIN_CTL% /p:DlssnrProfileDomainGpu=%DOMAIN_GPU% /p:DlssnrTraceEnabled=%TRACE_ENABLED% /p:DlssnrMutexCompatibility=%MUTEX_COMPAT% /p:VerifyShaders=%VERIFY_SHADERS% /p:PrecompileShaders=%PRECOMPILE_SHADERS% /v:minimal /nologo
if errorlevel 1 (
    echo.
    echo BUILD FAILED!
    echo   1. Retry %BUILD_TARGET%
    echo   2. Rebuild from scratch
    echo   3. Abort
    choice /C 123 /N /M "Choose [1-3]: "
    if errorlevel 3 goto :FINISH
    if errorlevel 2 (
        set "BUILD_TARGET=Rebuild"
    )
    cls
    goto :BUILD
)
echo.
echo Build succeeded.

REM ============================================================
REM  Copy next to the script (with retry).
REM ============================================================
:COPY_RETRY
echo.
echo [2/3] Copying addon to the script directory...
echo.

for %%I in ("%SOLUTION_PATH%") do set "SOLUTION_DIR=%%~dpI"
set "BUILT_ADDON=%SOLUTION_DIR%build\%CONFIG%\reshade_dlssnr64.addon64"

if not exist "%BUILT_ADDON%" (
    echo ERROR: Addon output not found at:
    echo   "%BUILT_ADDON%"
    choice /C YN /N /M "Retry copy? [Y/N]: "
    if errorlevel 2 goto :FINISH
    cls
    goto :COPY_RETRY
)

copy /Y "%BUILT_ADDON%" "%ADDON_OUTPUT%" >nul
if errorlevel 1 (
    echo COPY FAILED!
    choice /C YN /N /M "Retry copy? [Y/N]: "
    if errorlevel 2 goto :FINISH
    cls
    goto :COPY_RETRY
)
echo.
echo Copied to: %ADDON_OUTPUT%

REM ============================================================
REM  Launch. Never automatic: a build that ends by starting the
REM  game cannot be run in the background, and the wrong executable
REM  is easy to pick by accident.
REM ============================================================
echo.
echo [3/3] Launch...
if not defined TARGET_EXE (
    echo No launch target selected; nothing to start.
    goto :FINISH
)
set "LAUNCH_PATH=%SCRIPT_DIR%\%TARGET_EXE%"
if not exist "%LAUNCH_PATH%" (
    echo WARNING: launch target not found at:
    echo   "%LAUNCH_PATH%"
    goto :FINISH
)

set "RUN_NOW="
set /p "RUN_NOW=Launch !TARGET_EXE!? [y/N]: "
if not defined RUN_NOW goto :FINISH
if /i not "%RUN_NOW:~0,1%"=="y" goto :FINISH

start "" "%LAUNCH_PATH%"
echo Launched: %LAUNCH_PATH%

:FINISH
call :Persist
echo.
exit /b 0

REM ============================================================
REM  Subroutines. Kept above the persisted line so that rewriting
REM  it never shifts an offset cmd has yet to read.
REM ============================================================
:SetDefaults
set "BUILD_TARGET=Build"
set "CONFIG=Release"
set "TRACY_ENABLED=true"
set "PROFILE_VERBOSITY=1"
set "DOMAIN_CORE=1"
set "DOMAIN_RENDER=1"
set "DOMAIN_RESOURCE=1"
set "DOMAIN_ASYNC=1"
set "DOMAIN_UI=1"
set "DOMAIN_CTL=1"
set "DOMAIN_GPU=0"
set "TRACE_ENABLED=true"
set "MUTEX_COMPAT=true"
set "VERIFY_SHADERS=true"
set "PRECOMPILE_SHADERS=false"
set "TARGET_EXE="
exit /b 0

:SELECT_EXE
if %EXE_COUNT%==0 (
    echo.
    echo No .exe found in "%SCRIPT_DIR%".
    echo Place this script next to the game executable.
    set "TARGET_EXE="
    exit /b 0
)
cls
echo ============================================================
echo   Choose the launch target in
echo   %SCRIPT_DIR%
echo ============================================================
echo.
for /l %%i in (1,1,%EXE_COUNT%) do echo   %%i. !EXE_%%i!
echo   0. Do not launch anything
echo.
set "PICK="
set /p "PICK=Choose [0-%EXE_COUNT%]: "
REM Empty keeps whatever was remembered instead of looping on a closed input.
if not defined PICK exit /b 0
if "%PICK%"=="0" (
    set "TARGET_EXE="
    exit /b 0
)
set "PICKED="
for /l %%i in (1,1,%EXE_COUNT%) do if "%PICK%"=="%%i" set "PICKED=!EXE_%%i!"
if not defined PICKED (
    echo Invalid choice.
    echo.
    goto :SELECT_EXE
)
set "TARGET_EXE=%PICKED%"
echo.
echo Launch target set to: %TARGET_EXE%
exit /b 0

:ASK_DOMAIN
REM %1 = display name, %2 = variable, %3 = default (Y/N)
set "DOMAIN_ANSWER="
set /p "DOMAIN_ANSWER=%~1 zones? [%3]: "
if not defined DOMAIN_ANSWER set "DOMAIN_ANSWER=%3"
if /i "%DOMAIN_ANSWER:~0,1%"=="y" (
    set "%~2=1"
) else (
    set "%~2=0"
)
exit /b 0

:Persist
set "NEW_CFG=REM CFG=%BUILD_TARGET% %CONFIG% %TRACY_ENABLED% %PROFILE_VERBOSITY% %DOMAIN_CORE% %DOMAIN_RENDER% %DOMAIN_RESOURCE% %DOMAIN_ASYNC% %DOMAIN_UI% %DOMAIN_CTL% %DOMAIN_GPU% %TRACE_ENABLED% %MUTEX_COMPAT% %VERIFY_SHADERS% %PRECOMPILE_SHADERS%"
set "PERSIST_TMP=%TEMP%\dlssnr_build_rewrite_%RANDOM%%RANDOM%.bat"
findstr /v /b /c:"REM CFG=" /c:"REM TARGET_EXE=" "%~f0" > "%PERSIST_TMP%"
>> "%PERSIST_TMP%" echo %NEW_CFG%
>> "%PERSIST_TMP%" echo REM TARGET_EXE=%TARGET_EXE%
move /y "%PERSIST_TMP%" "%~f0" >nul 2>nul
exit /b 0

REM CFG=
REM TARGET_EXE=
