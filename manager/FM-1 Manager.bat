@echo off
rem Windows: double-click to start the FM-1 Manager (the first start sets it up)
cd /d "%~dp0"
where py >nul 2>nul && (set PY=py -3) || (set PY=python)
if not exist .venv\Scripts\pythonw.exe (
  echo Setting up ^(once^) ...
  %PY% -m venv .venv || goto nopy
  .venv\Scripts\python -m pip install --quiet --disable-pip-version-check -r requirements.txt || goto fail
)
start "" .venv\Scripts\pythonw.exe fm1_manager.py
exit /b 0
:nopy
echo Python 3 is needed: install it from https://www.python.org/downloads/ ^(tick "Add python.exe to PATH"^), then start this again.
pause
exit /b 1
:fail
echo Setup failed, see above.
rmdir /s /q .venv
pause
exit /b 1
