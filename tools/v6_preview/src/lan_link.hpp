// The V6 pipeline over the product's LAN video stream (docs/spec/lan-media.md 7) instead of the TCP
// test link: the sender follows the engine's subscriptions like capd will, the viewer pairs with the
// engine and subscribes like the remote UI will. For the P3 lane N tests on real PCs.
//
//   dc-v6-bench --send-lan [ENGINE]     on the engine PC (mixd running): test pattern -> viewers
//   dc-v6-preview --engine HOST[:PORT] [--pair CODE]   on the remote device
#pragma once

#include "mf_codec.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dc::lan {
class VideoOutput;
class VideoInput;
}  // namespace dc::lan

namespace v6 {

class LanVideoSender {
public:
    // engine "host:port" of mixd on this PC; stream "video/preview" or "video/lan".
    LanVideoSender(const std::string& engine, std::string stream, std::uint16_t videoPort);
    ~LanVideoSender();

    void send(const EncodedUnit& unit);  // encoder output thread
    bool keyframeWanted() const;
    std::string status() const;

private:
    void run();

    std::string host_;
    int port_ = 0;
    std::string stream_;
    std::unique_ptr<dc::lan::VideoOutput> out_;
    std::atomic<int> viewers_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

class LanVideoViewer {
public:
    struct Options {
        std::string engine;  // host[:port]
        std::string stream = "video/preview";
        std::string pairCode;  // first time only; the token is kept in %APPDATA%\DesktopCapture
    };
    LanVideoViewer(Options options, std::function<void(EncodedUnit&&)> onUnit);
    ~LanVideoViewer();

    std::string status() const;

private:
    void run();

    Options o_;
    std::string host_;
    int port_ = 0;
    std::function<void(EncodedUnit&&)> onUnit_;
    std::unique_ptr<dc::lan::VideoInput> in_;
    mutable std::mutex mu_;
    std::string state_ = "connecting";
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace v6
