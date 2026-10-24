#include "engine.hpp"

#include "dc/lanmedia/audio_receiver.hpp"
#include "dc/lanmedia/audio_sender.hpp"
#include "dc/shmaudio/clock.hpp"

#include <algorithm>
#include <cstring>

#if defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#define DC_HAVE_MXCSR 1
#endif

namespace dc::mixd {

namespace {

// Flush denormals to zero while rendering: the tails of reverb, delay and the filters decay into
// denormal floats, which cost 10-100x more CPU each on x86 (and plugins expect a host to do this).
class DenormalGuard {
public:
#ifdef DC_HAVE_MXCSR
    DenormalGuard() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040); } // FTZ | DAZ
    ~DenormalGuard() { _mm_setcsr(saved_); }

private:
    unsigned saved_;
#endif
};

} // namespace

double qpcSecondsNow() {
    static const double freq = static_cast<double>(shm::qpcFrequency());
    return static_cast<double>(shm::nowQpc()) / freq;
}

Engine::Engine(shm::AudioRingWriter* shm) : shm_(shm) {}

Engine::~Engine() {
    collectRetired();
    delete pending_.exchange(nullptr);
    delete current_;
}

void Engine::setPlan(std::unique_ptr<MixPlan> plan) {
    // the audio thread only ever takes pending_ with exchange, so an unadopted plan can be freed here
    delete pending_.exchange(plan.release(), std::memory_order_acq_rel);
}

void Engine::collectRetired() {
    for (auto& slot : retired_)
        delete slot.exchange(nullptr, std::memory_order_acq_rel);
}

void Engine::adoptPending() {
    if (!pending_.load(std::memory_order_relaxed)) return;
    std::atomic<MixPlan*>* free = nullptr;
    if (current_) {
        for (auto& slot : retired_)
            if (!slot.load(std::memory_order_acquire)) {
                free = &slot;
                break;
            }
        if (!free) return; // nobody collected the old plans yet: keep the current one a little longer
    }
    MixPlan* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (!next) return;
    if (free) free->store(current_, std::memory_order_release);
    current_ = next;
    adopted_.store(current_->generation, std::memory_order_release);
}

void Engine::render(const OutputPort* clockPort, float* out, int frames, int outChannels, double time) {
    const double start = qpcSecondsNow();
    const DenormalGuard denormals;
    adoptPending();
    if (out) std::memset(out, 0, static_cast<std::size_t>(frames) * static_cast<std::size_t>(outChannels) * sizeof(float));
    const MixPlan* plan = current_;
    if (!plan) return;

    const std::int64_t qpcFreq = shm::qpcFrequency();
    int done = 0;
    while (done < frames) {
        const int n = std::min({frames - done, plan->blockFrames, kMaxBlockFrames});
        const double chunkTime = time + done / kSampleRate;

        for (std::size_t i = 0; i < plan->inputs.size(); ++i) {
            auto& port = *plan->inputs[i];
            float* buf = plan->inputScratch[i].data();
            if (port.lan) port.lan->pull(buf, static_cast<std::size_t>(n), chunkTime);
            else port.bridge.pull(buf, static_cast<std::size_t>(n), chunkTime);
            inputPtrs_[i] = buf;
        }
        mixer_.process(*plan, n, inputPtrs_.data(), shm_, start + done / kSampleRate);

        for (std::size_t b = 0; b < plan->buses.size(); ++b) {
            const float* l = mixer_.bus(static_cast<int>(b), 0);
            const float* r = mixer_.bus(static_cast<int>(b), 1);
            if (plan->buses[b].get() == clockPort) {
                if (!out) continue;
                float* dst = out + static_cast<std::ptrdiff_t>(done) * outChannels;
                if (outChannels == 1) {
                    for (int i = 0; i < n; ++i) dst[i] += (l[i] + r[i]) * 0.5f;
                } else {
                    for (int i = 0; i < n; ++i) {
                        dst[i * outChannels] += l[i];
                        dst[i * outChannels + 1] += r[i];
                    }
                }
            } else {
                for (int i = 0; i < n; ++i) {
                    interleave_[static_cast<std::size_t>(2 * i)] = l[i];
                    interleave_[static_cast<std::size_t>(2 * i + 1)] = r[i];
                }
                auto& port = *plan->buses[b];
                if (port.lan) port.lan->push(interleave_.data(), static_cast<std::size_t>(n), chunkTime);
                else port.bridge.push(interleave_.data(), static_cast<std::size_t>(n), chunkTime);
            }
        }

        if (shm_) {
            const auto qpc = static_cast<std::int64_t>((start + done / kSampleRate) * static_cast<double>(qpcFreq));
            shm_->commit(static_cast<std::uint32_t>(n), qpc);
        }
        done += n;
    }
    frames_.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);

    // load = processing time / audio time, reported as the peak over ~0.5 s
    const double used = qpcSecondsNow() - start;
    loadPeak_ = std::max(loadPeak_, used / (frames / kSampleRate));
    loadFrames_ += static_cast<std::uint64_t>(frames);
    if (loadFrames_ >= 24000) {
        load_.store(std::min(loadPeak_, 1.0), std::memory_order_relaxed);
        loadPeak_ = 0;
        loadFrames_ = 0;
    }
}

} // namespace dc::mixd
