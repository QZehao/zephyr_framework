@echo off
REM =============================================================================
REM Proprietary Modules Management Script (Windows Batch Wrapper)
REM =============================================================================
REM Usage:
REM   proprietary_manage status    Show status
REM   proprietary_manage enable    Enable proprietary modules
REM   proprietary_manage disable   Disable proprietary modules
REM   proprietary_manage update    Update proprietary modules
REM =============================================================================

set SCRIPT_DIR=%~dp0
set PS_SCRIPT=%SCRIPT_DIR%proprietary_manage.ps1

if "%1"=="" (
    powershell -ExecutionPolicy Bypass -File "%PS_SCRIPT%" -Help
    exit /b 0
)

if /i "%1"=="status" (
    powershell -ExecutionPolicy Bypass -File "%PS_SCRIPT%" -Status
) else if /i "%1"=="enable" (
    powershell -ExecutionPolicy Bypass -File "%PS_SCRIPT%" -Enable
) else if /i "%1"=="disable" (
    powershell -ExecutionPolicy Bypass -File "%PS_SCRIPT%" -Disable
) else if /i "%1"=="update" (
    powershell -ExecutionPolicy Bypass -File "%PS_SCRIPT%" -Update
) else if /i "%1"=="help" (
    powershell -ExecutionPolicy Bypass -File "%PS_SCRIPT%" -Help
) else if /i "%1"=="-h" (
    powershell -ExecutionPolicy Bypass -File "%PS_SCRIPT%" -Help
) else if /i "%1"=="--help" (
    powershell -ExecutionPolicy Bypass -File "%PS_SCRIPT%" -Help
) else (
    echo Unknown command: %1
    echo.
    echo Usage: proprietary_manage [status^|enable^|disable^|update^|help]
    exit /b 1
)
