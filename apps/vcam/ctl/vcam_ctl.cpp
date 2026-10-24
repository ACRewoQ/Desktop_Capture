// dc-vcam-ctl: creates / removes the Media Foundation virtual camera (Windows 11).
//
//   dc-vcam-ctl install             camera persists for the current user until `uninstall` (normal use)
//   dc-vcam-ctl uninstall
//   dc-vcam-ctl run [--seconds N]   camera exists while this process runs (session lifetime, tests)
//   dc-vcam-ctl supported           prints whether this Windows supports MF virtual cameras
//
// The source DLL (vcam_mf.dll) must already be registered in HKLM (scripts/vcam-register.ps1, which also runs
// "install"). None of these commands needs elevation (current-user access).
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwchar>
#include <string>

#include "dc/vcam/ids.hpp"

using Microsoft::WRL::ComPtr;

namespace {

volatile bool g_stop = false;

BOOL WINAPI on_ctrl(DWORD) {
    g_stop = true;
    return TRUE;
}

HRESULT create(MFVirtualCameraLifetime lifetime, IMFVirtualCamera** out) {
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, lifetime, MFVirtualCameraAccess_CurrentUser,
                                 DC_VCAM_MF_NAME, DC_VCAM_MF_CLSID_WSTR, nullptr, 0, out);
}

int fail(const char* what, HRESULT hr) {
    std::printf("ERROR %s failed: 0x%08lx\n", what, hr);
    return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring cmd = argc > 1 ? argv[1] : L"";
    double seconds = 0;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::wstring(argv[i]) == L"--seconds") seconds = _wtof(argv[i + 1]);

    // mfsensorgroup.dll is delay-loaded: on Windows 10 it lacks the virtual camera API, and the
    // tool should say so instead of failing to start.
    const HMODULE sg = LoadLibraryW(L"mfsensorgroup.dll");
    if (!sg || !GetProcAddress(sg, "MFCreateVirtualCamera")) {
        std::printf("MF virtual camera supported: no (this Windows has no MFCreateVirtualCamera)\n");
        return cmd == L"supported" ? 1 : 3;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    int rc = 0;

    if (cmd == L"supported") {
        BOOL ok = FALSE;
        const HRESULT hr = MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &ok);
        std::printf("MF virtual camera supported: %s (hr 0x%08lx)\n", (SUCCEEDED(hr) && ok) ? "yes" : "no", hr);
        rc = (SUCCEEDED(hr) && ok) ? 0 : 1;
    } else if (cmd == L"run") {
        ComPtr<IMFVirtualCamera> cam;
        HRESULT hr = create(MFVirtualCameraLifetime_Session, &cam);
        if (FAILED(hr)) return fail("MFCreateVirtualCamera", hr);
        hr = cam->Start(nullptr);
        if (FAILED(hr)) return fail("IMFVirtualCamera::Start", hr);
        std::printf("camera \"%ls\" started; %s\n", DC_VCAM_MF_NAME,
                    seconds > 0 ? "running for the given seconds" : "Ctrl+C to remove it");
        std::fflush(stdout);
        SetConsoleCtrlHandler(on_ctrl, TRUE);
        const ULONGLONG until = GetTickCount64() + ULONGLONG(seconds * 1000);
        while (!g_stop && (seconds <= 0 || GetTickCount64() < until)) Sleep(100);
        cam->Stop();
        cam->Remove();
        cam->Shutdown();
        std::printf("camera removed\n");
    } else if (cmd == L"install") {
        ComPtr<IMFVirtualCamera> cam;
        HRESULT hr = create(MFVirtualCameraLifetime_System, &cam);
        if (FAILED(hr)) return fail("MFCreateVirtualCamera", hr);
        hr = cam->Start(nullptr);
        if (FAILED(hr)) return fail("IMFVirtualCamera::Start", hr);
        cam->Shutdown();
        std::printf("camera \"%ls\" installed (stays until `uninstall`)\n", DC_VCAM_MF_NAME);
    } else if (cmd == L"uninstall") {
        ComPtr<IMFVirtualCamera> cam;
        HRESULT hr = create(MFVirtualCameraLifetime_System, &cam);
        if (FAILED(hr)) return fail("MFCreateVirtualCamera", hr);
        hr = cam->Remove();
        cam->Shutdown();
        if (FAILED(hr)) return fail("IMFVirtualCamera::Remove", hr);
        std::printf("camera removed\n");
    } else {
        std::printf("usage: dc-vcam-ctl install | uninstall | run [--seconds N] | supported\n");
        rc = 2;
    }
    MFShutdown();
    CoUninitialize();
    return rc;
}
