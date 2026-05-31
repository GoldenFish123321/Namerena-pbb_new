@echo off
REM PBB 一键启动 — Windows
python --version >nul 2>&1 || (echo ERROR: Python not found. Install from https://python.org & pause & exit /b 1)
cd /d "%~dp0"
python main.py %*
if %errorlevel% neq 0 pause
