// Identities of the two virtual cameras. Windows 11 registers only the Media Foundation camera (it reaches
// DirectShow apps too); Windows 10 only the DirectShow one (V4 finding 6).
#pragma once

#include <guiddef.h>

// Media Foundation virtual camera source (Windows 11). Windows shows it as "<name> (Windows 仮想カメラ)".
#define DC_VCAM_MF_CLSID_WSTR L"{EEE660E0-69C0-49E2-827E-7CD7B9ADC85D}"
#define DC_VCAM_MF_NAME L"Desktop Capture Camera"

// DirectShow source filter (Windows 10)
#define DC_VCAM_DS_NAME L"Desktop Capture Camera"

namespace dc::vcam {
inline constexpr GUID kMfClsid = {0xeee660e0, 0x69c0, 0x49e2, {0x82, 0x7e, 0x7c, 0xd7, 0xb9, 0xad, 0xc8, 0x5d}};
inline constexpr GUID kDsClsid = {0x08b80054, 0xfa38, 0x4ab9, {0x97, 0x43, 0xef, 0x1f, 0xd6, 0xa3, 0x8d, 0x24}};

// Windows 11 starts at build 22000; MFCreateVirtualCamera exists from there.
inline constexpr unsigned long kFirstMfVirtualCameraBuild = 22000;

// The DirectShow camera always offers this one format (a camera app opened before capd runs still lists it).
inline constexpr int kDsWidth = 1920;
inline constexpr int kDsHeight = 1080;
}  // namespace dc::vcam
