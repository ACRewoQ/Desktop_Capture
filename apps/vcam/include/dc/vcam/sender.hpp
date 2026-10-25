// Producer side of the virtual camera, used by capd (and dc-vcam-probe's test feed). Windows only.
//
// Windows 11: writes NV12 (or BGRA) frames into the bridge section the MF source creates in the Frame Server.
//   The section exists only while a camera app has the camera open, so attaching is retried twice a second.
// Windows 10: hands BGR24 frames to the DirectShow filter's sender API (softcam), loaded from the installed
//   vcam_ds.dll (%ProgramData%\DesktopCapture\vcam\current.txt names the install folder). The filter always
//   offers 1920x1080, so the producer sends exactly that size.
#pragma once

#include "dc/vcam/bridge.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace dc::vcam {

enum class CameraKind {
    MediaFoundation,  // Windows 11 (build 22000 and later)
    DirectShow,       // Windows 10
};

// The camera this Windows uses (the register script installs only that one).
CameraKind systemCameraKind();

// Not thread-safe: call every method from the same thread (capd: the video thread).
class Sender {
public:
    struct Status {
        bool installed = false;  // DirectShow: vcam_ds.dll found and loaded; MF: always true (see attached)
        bool attached = false;   // MF: bridge section open (a camera app opened the camera); DS: sender created
        bool consumer = false;   // an app is reading frames right now
        std::uint64_t sent = 0;
        std::string error;       // why the camera cannot be fed ("not installed" ...)
    };

    explicit Sender(CameraKind kind);
    ~Sender();
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    CameraKind kind() const { return kind_; }

    // MF only. NV12: y plane then uv plane, each with its own pitch. width/height even, at most 1920x1080.
    void sendNv12(const std::uint8_t* y, int yPitch, const std::uint8_t* uv, int uvPitch, int width, int height,
                  std::int64_t qpc);
    // Both kinds. DirectShow needs 1920x1080 (other sizes are dropped and reported in Status::error).
    void sendBgra(const std::uint8_t* bgra, int pitch, int width, int height, std::int64_t qpc);

    // Tells the camera the producer is gone (it switches to the "no signal" picture at once).
    void detach();

    Status status() const;

private:
    struct Mf;
    struct Ds;
    CameraKind kind_;
    std::unique_ptr<Mf> mf_;
    std::unique_ptr<Ds> ds_;
};

}  // namespace dc::vcam
