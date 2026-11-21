@echo off
setlocal
if not defined HARMONY_STUDIO set "HARMONY_STUDIO=%ProgramFiles%\Huawei\DevEco Studio"
set "studioPython=%HARMONY_STUDIO%\plugins\app-analyzer\lib\python\python.exe"
if exist "%studioPython%" goto studio
py -3 -c "import sys; sys.exit(sys.version_info < (3,10))" >nul 2>nul
if not errorlevel 1 goto launcher
python -c "import sys; sys.exit(sys.version_info < (3,10))" >nul 2>nul
if not errorlevel 1 goto python
echo Install Python 3.10+ or set HARMONY_STUDIO to the DevEco Studio directory.
pause
exit /b 1
:studio
"%studioPython%" -B "%~dp0portable.py" %*
goto result
:launcher
py -3 -B "%~dp0portable.py" %*
goto result
:python
python -B "%~dp0portable.py" %*
:result
set "result=%errorlevel%"
if "%~1"=="" pause
exit /b %result%
