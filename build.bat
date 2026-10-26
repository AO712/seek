@echo off
rem Build Seek with MinGW-w64 (MSYS2: pacman -S mingw-w64-x86_64-gcc)
windres seek.rc -O coff -o seek.res || exit /b 1
g++ -std=c++17 -Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -s -static -mwindows -municode ^
    main.cpp seek.res -o Seek.exe -ld2d1 -ldwrite -ldwmapi -lshlwapi -lole32 -luuid -lshell32 -luser32 -lgdi32 -ladvapi32
