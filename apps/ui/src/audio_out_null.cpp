#include "audio_out.hpp"

#include "dc/lanmedia/udp_transport.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace dc::ui {

namespace {

class NullOut final : public AudioOut {
public:
    explicit NullOut(Render render) : render_(std::move(render)) {
        thread_ = std::thread([this] {
            constexpr std::size_t kFrames = 480;
            std::vector<float> buf(kFrames * 2);
            auto next = std::chrono::steady_clock::now();
            while (running_) {
                render_(buf.data(), kFrames, lan::nowSeconds());
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
            }
        });
    }
    ~NullOut() override {
        running_ = false;
        thread_.join();
    }
    QString deviceName() const override { return QStringLiteral("（出力なし）"); }
    QString error() const override { return {}; }

private:
    Render render_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

} // namespace

std::unique_ptr<AudioOut> makeNullAudioOut(AudioOut::Render render) { return std::make_unique<NullOut>(std::move(render)); }

#ifndef _WIN32
std::unique_ptr<AudioOut> AudioOut::create(Render render, bool) { return makeNullAudioOut(std::move(render)); }
#endif

} // namespace dc::ui
