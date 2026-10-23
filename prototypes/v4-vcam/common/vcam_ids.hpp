// V4 prototype: identities of the two virtual cameras.
#pragma once

#include <guiddef.h>

// Media Foundation virtual camera source (Windows 11)
#define DC_VCAM_MF_CLSID_WSTR L"{EEE660E0-69C0-49E2-827E-7CD7B9ADC85D}"
#define DC_VCAM_MF_NAME L"Desktop Capture Camera"

// DirectShow source filter (Windows 10, and DirectShow apps on Windows 11)
#define DC_VCAM_DS_NAME L"Desktop Capture Camera (DirectShow)"

namespace dc::vcam {
inline constexpr GUID kMfClsid = {0xeee660e0, 0x69c0, 0x49e2, {0x82, 0x7e, 0x7c, 0xd7, 0xb9, 0xad, 0xc8, 0x5d}};
inline constexpr GUID kDsClsid = {0x08b80054, 0xfa38, 0x4ab9, {0x97, 0x43, 0xef, 0x1f, 0xd6, 0xa3, 0x8d, 0x24}};
}  // namespace dc::vcam
