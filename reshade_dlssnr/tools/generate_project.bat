@echo off
setlocal
set "PYTHON=py -3"
where py >nul 2>nul || set "PYTHON=python"
%PYTHON% "%~dp0generate_project.py" %*
set "RC=%ERRORLEVEL%"
if "%RC%"=="0" (
    echo [OK] generate_project.py exited with code 0
) else (
    echo [FAIL] generate_project.py exited with code %RC%
)
pause
exit /b %RC%
