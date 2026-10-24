// dc-av-flash: turns the top-left quarter of the primary monitor white for a moment at every multiple
// of PERIOD seconds of QPC time, and black otherwise. mixd --test-pulse PERIOD sounds a burst at the
// same instants, so a recording made meanwhile shows how far its audio sits from its picture
// (capd avsync FILE). Esc, --seconds or the --stop-file appearing ends it.
//
//   dc-av-flash [--period SEC] [--seconds N] [--stop-file PATH] [--flash-ms MS] [--size FRACTION]

#include <windows.h>
#include <mmsystem.h> // timeBeginPeriod (not in WIN32_LEAN_AND_MEAN)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool g_white = false;

double qpcSeconds() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(g_white ? WHITE_BRUSH : BLACK_BRUSH)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int usage() {
    std::fprintf(stderr, "usage: dc-av-flash [--period SEC] [--seconds N] [--stop-file PATH] [--flash-ms MS] [--size FRACTION]\n"
                         "  --period    seconds between flashes, same as mixd --test-pulse (default 2)\n"
                         "  --seconds   stop after N seconds (default: until Esc)\n"
                         "  --stop-file stop when this file exists (checked every flash)\n"
                         "  --flash-ms  how long the corner stays white (default 200)\n"
                         "  --size      window size as a fraction of the monitor (default 0.25)\n");
    return 2;
}

void setColour(HWND hwnd, bool white) {
    g_white = white;
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd); // paints now, not when the message loop gets to it
}

} // namespace

int main(int argc, char** argv) {
    double period = 2.0, seconds = 0, flashMs = 200, size = 0.25;
    std::string stopFile;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> double {
            if (i + 1 >= argc) std::exit(usage());
            return std::atof(argv[++i]);
        };
        if (!std::strcmp(argv[i], "--period")) period = next();
        else if (!std::strcmp(argv[i], "--seconds")) seconds = next();
        else if (!std::strcmp(argv[i], "--flash-ms")) flashMs = next();
        else if (!std::strcmp(argv[i], "--size")) size = next();
        else if (!std::strcmp(argv[i], "--stop-file") && i + 1 < argc) stopFile = argv[++i];
        else return usage();
    }
    if (period <= 0.1 || flashMs <= 0 || flashMs >= period * 1000 || size <= 0 || size > 1) return usage();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = L"DcAvFlash";
    RegisterClassW(&wc);

    // Top-left corner of the primary monitor (capd records the primary monitor by default).
    const int w = static_cast<int>(GetSystemMetrics(SM_CXSCREEN) * size);
    const int h = static_cast<int>(GetSystemMetrics(SM_CYSCREEN) * size);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"dc-av-flash", WS_POPUP, 0, 0, w,
                                h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "cannot create the window (%lu)\n", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    setColour(hwnd, false);
    std::printf("dc-av-flash: %dx%d at the top-left, flash every %.3f s of QPC time for %.0f ms. Esc to stop.\n", w,
                h, period, flashMs);
    std::fflush(stdout);

    const double start = qpcSeconds();
    double nextFlash = (std::floor(start / period) + 1) * period;
    double flashEnd = 0;
    std::vector<double> lateMs; // how long after the instant the white frame was painted
    MSG msg;
    bool running = true;
    while (running) {
        const double now = qpcSeconds();
        if (seconds > 0 && now - start >= seconds) break;
        const double due = g_white ? flashEnd : nextFlash;
        const double wait = due - now;
        // Sleep until 2 ms before the instant (waking for window messages), then spin.
        if (wait > 0.002) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>((wait - 0.002) * 1000), QS_ALLINPUT);
        } else if (wait > 0) {
            continue;
        } else if (!g_white) {
            setColour(hwnd, true);
            lateMs.push_back((qpcSeconds() - nextFlash) * 1000);
            flashEnd = nextFlash + flashMs / 1000;
            nextFlash += period;
            if (nextFlash < qpcSeconds()) nextFlash = (std::floor(qpcSeconds() / period) + 1) * period; // fell behind
        } else {
            setColour(hwnd, false);
            if (!stopFile.empty() && GetFileAttributesA(stopFile.c_str()) != INVALID_FILE_ATTRIBUTES) break;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(hwnd);
    timeEndPeriod(1);

    if (!lateMs.empty()) {
        std::sort(lateMs.begin(), lateMs.end());
        std::printf("RESULT flashes=%zu paint_late_ms_median=%.2f paint_late_ms_max=%.2f\n", lateMs.size(),
                    lateMs[lateMs.size() / 2], lateMs.back());
        std::fflush(stdout);
    }
    return 0;
}
