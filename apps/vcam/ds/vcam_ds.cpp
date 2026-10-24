// DirectShow virtual camera (Windows 10; Windows 11 uses the Media Foundation camera, which DirectShow apps see
// too). From the V4 verification.
//
// Built on softcam (https://github.com/tshino/softcam, MIT): its DirectShow source filter and
// sender API are compiled from the fetched sources; this file only gives the filter our own
// CLSID and name and registers it in the video input device category. The shared memory names
// are patched at configure time (see CMakeLists.txt) so this never talks to a stock softcam, and the filter always
// offers 1920x1080 30 fps with a "no signal" picture while capd is not sending (V4 finding 8).
//
// Unlike the MF camera, the DirectShow filter runs inside the camera app's own process, so the
// producer and the filter meet in the session-local namespace and no service is involved.
#include <softcam/softcam.h>

#include <olectl.h>

#include <softcamcore/DShowSoftcam.h>
#include <softcamcore/SenderAPI.h>

#include "dc/vcam/ids.hpp"

namespace {

const wchar_t kFilterName[] = DC_VCAM_DS_NAME;
const GUID& kFilterClsid = dc::vcam::kDsClsid;

const AMOVIESETUP_MEDIATYPE kPinTypes[] = {{&MEDIATYPE_Video, &MEDIASUBTYPE_NULL}};

const AMOVIESETUP_PIN kPins[] = {{const_cast<LPWSTR>(L"Output"), FALSE, TRUE, FALSE, FALSE, &CLSID_NULL, nullptr, 1,
                                  kPinTypes}};

const REGFILTER2 kRegFilter2 = {1, MERIT_DO_NOT_USE, 1, kPins};

CUnknown* WINAPI create_instance(LPUNKNOWN outer, HRESULT* hr) {
    return softcam::Softcam::CreateInstance(outer, kFilterClsid, hr);
}

HRESULT register_filter(bool reg) {
    HRESULT hr = AMovieDllRegisterServer2(reg ? TRUE : FALSE);
    if (FAILED(hr)) return hr;
    hr = CoInitialize(nullptr);
    if (FAILED(hr)) return hr;
    IFilterMapper2* fm = nullptr;
    hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                          reinterpret_cast<void**>(&fm));
    if (SUCCEEDED(hr)) {
        fm->UnregisterFilter(&CLSID_VideoInputDeviceCategory, kFilterName, kFilterClsid);
        if (reg)
            hr = fm->RegisterFilter(kFilterClsid, kFilterName, nullptr, &CLSID_VideoInputDeviceCategory, kFilterName,
                                    &kRegFilter2);
        fm->Release();
    }
    CoFreeUnusedLibraries();
    CoUninitialize();
    return hr;
}

}  // namespace

CFactoryTemplate g_Templates[] = {{kFilterName, &kFilterClsid, &create_instance, nullptr, nullptr}};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

STDAPI DllRegisterServer() { return register_filter(true); }
STDAPI DllUnregisterServer() { return register_filter(false); }

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);

BOOL APIENTRY DllMain(HANDLE module, DWORD reason, LPVOID reserved) {
    return DllEntryPoint(static_cast<HINSTANCE>(module), reason, reserved);
}

// Sender API (used by capd, which loads this same DLL from the install folder)
extern "C" scCamera scCreateCamera(int width, int height, float framerate) {
    return softcam::sender::CreateCamera(width, height, framerate);
}
extern "C" void scDeleteCamera(scCamera camera) { softcam::sender::DeleteCamera(camera); }
extern "C" void scSendFrame(scCamera camera, const void* image_bits) { softcam::sender::SendFrame(camera, image_bits); }
extern "C" bool scWaitForConnection(scCamera camera, float timeout) {
    return softcam::sender::WaitForConnection(camera, timeout);
}
extern "C" bool scIsConnected(scCamera camera) { return softcam::sender::IsConnected(camera); }
