@rem Builds dc-loopback-probe.exe with MinGW-w64 g++ (e.g. a portable w64devkit) on a PC without Visual Studio.
@rem Usage: build-mingw.cmd   (g++ must be on PATH; output: dc-loopback-probe.exe next to this file)
cd /d "%~dp0"
g++ -std=c++20 -O2 -s -static -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 main.cpp loopback_capture.cpp tone.cpp -o dc-loopback-probe.exe -lole32 -lavrt -lmmdevapi -luuid
