#include "mixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dc::mixd {

namespace {

void storeMax(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

void storeAdd(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {
    }
}

void addScaled(float* dst, const float* src, int frames) {
    for (int i = 0; i < frames; ++i) dst[i] += src[i];
}

} // namespace

void MeterBank::add(int strip, const float* const* samples, int channels, int frames) {
    auto& a = accum_[strip];
    for (int c = 0; c < channels; ++c) {
        float peak = 0, sum = 0;
        for (int i = 0; i < frames; ++i) {
            const float v = samples[c][i];
            peak = std::max(peak, std::fabs(v));
            sum += v * v;
        }
        storeMax(a.peak[c], peak);
        storeAdd(a.sumSq[c], sum);
    }
    a.frames.fetch_add(static_cast<std::uint32_t>(frames), std::memory_order_release);
}

MeterBank::Reading MeterBank::take(int strip) {
    auto& a = accum_[strip];
    Reading r;
    const auto frames = a.frames.exchange(0, std::memory_order_acquire);
    for (int c = 0; c < 2; ++c) {
        r.peak[c] = a.peak[c].exchange(0, std::memory_order_relaxed);
        const float sum = a.sumSq[c].exchange(0, std::memory_order_relaxed);
        r.rms[c] = frames ? std::sqrt(sum / static_cast<float>(frames)) : 0.0f;
        r.clipped = r.clipped || r.peak[c] >= 1.0f;
    }
    r.valid = frames > 0;
    return r;
}

Mixer::Mixer()
    : strips_(std::make_unique<std::array<StripState, kMaxStrips>>()),
      acc_(std::make_unique<std::array<std::array<Buffer, 2>, kMaxStrips>>()),
      busesStorage_(std::make_unique<std::array<std::array<Buffer, 2>, kMaxOutputBuses>>()),
      buses_(*busesStorage_) {
    for (auto& s : *acc_)
        for (auto& b : s) b.fill(0);
    for (auto& s : buses_)
        for (auto& b : s) b.fill(0);
}

void Mixer::initStrip(const StripPlan& s) {
    auto& st = (*strips_)[s.id];
    // a new strip fades in from silence; its filters start in their requested state
    st.gain.reset(0);
    st.gain.setTarget(s.gain);
    const auto pg = panGains(s.pan, s.channels == 2);
    st.panLeft.reset(pg.left);
    st.panRight.reset(pg.right);
    st.lowcut.reset(s.lowcut, s.lowcutHz);
    st.highcut.reset(s.highcut, s.highcutHz);
    st.sendPresent.fill(false);
    st.present = true;
}

void Mixer::adopt(const MixPlan& plan) {
    std::array<bool, kMaxStrips> inPlan{};
    inPlan[0] = true;
    for (const auto& t : plan.tracks) inPlan[t.id] = true;
    for (int i = 0; i < kMaxStrips; ++i)
        if (!inPlan[i]) (*strips_)[i].present = false;
    auto touch = [&](const StripPlan& s) {
        auto& st = (*strips_)[s.id];
        if (!st.present) initStrip(s);
        std::array<bool, 8> used{};
        for (const auto& send : s.sends) {
            used[send.slot] = true;
            if (!st.sendPresent[send.slot]) {
                st.sends[send.slot].reset(0); // fade in
                st.sendPresent[send.slot] = true;
            }
        }
        for (int i = 0; i < 8; ++i)
            if (!used[i]) st.sendPresent[i] = false;
    };
    touch(plan.master);
    for (const auto& t : plan.tracks) touch(t);
    generation_ = plan.generation;
}

float* Mixer::destination(const Destination& d, int channel) {
    switch (d.kind) {
        case Destination::Kind::Track: return (*acc_)[d.index][channel].data();
        case Destination::Kind::Master: return (*acc_)[0][channel].data();
        case Destination::Kind::Bus: return buses_[d.index][channel].data();
    }
    return nullptr;
}

void Mixer::writeShm(shm::AudioRingWriter* shm, int slot, const float* const* x, int channels, int frames) {
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) interleave_[static_cast<std::size_t>(i * channels + c)] = x[c][i];
    shm->write(static_cast<std::uint32_t>(slot), interleave_.data(), static_cast<std::uint32_t>(frames),
               static_cast<std::uint32_t>(channels));
}

void Mixer::process(const MixPlan& plan, int frames, const float* const* inputs, shm::AudioRingWriter* shm) {
    frames = std::clamp(frames, 0, kMaxBlockFrames);
    if (plan.generation != generation_) adopt(plan);

    const auto bytes = static_cast<std::size_t>(frames) * sizeof(float);
    for (int c = 0; c < 2; ++c) {
        std::memset((*acc_)[0][c].data(), 0, bytes);
        std::memset(cue_[c].data(), 0, bytes);
        for (const auto& t : plan.tracks) std::memset((*acc_)[t.id][c].data(), 0, bytes);
        for (std::size_t b = 0; b < plan.buses.size(); ++b) std::memset(buses_[b][c].data(), 0, bytes);
    }

    for (const auto& t : plan.tracks) processStrip(t, frames, inputs, plan, shm);

    if (plan.testToneGain > 0) {
        const double step = 2 * kPi * plan.testToneHz / kSampleRate;
        for (int i = 0; i < frames; ++i) {
            const float v = plan.testToneGain * static_cast<float>(std::sin(tonePhase_));
            (*acc_)[0][0][i] += v;
            (*acc_)[0][1][i] += v;
            tonePhase_ += step;
        }
        tonePhase_ = std::fmod(tonePhase_, 2 * kPi);
    }
    processStrip(plan.master, frames, inputs, plan, shm);

    if (plan.cueBus >= 0)
        for (int c = 0; c < 2; ++c) {
            float* dst = buses_[plan.cueBus][c].data();
            for (int i = 0; i < frames; ++i) dst[i] += cue_[c][i] * plan.cueGain;
        }
}

void Mixer::processStrip(const StripPlan& s, int frames, const float* const* inputs, const MixPlan& plan,
                         shm::AudioRingWriter* shm) {
    auto& st = (*strips_)[s.id];
    const int ch = s.id == 0 ? 2 : s.channels;
    float* x[2] = {work_[0].data(), work_[1].data()};
    const auto& in = (*acc_)[s.id];

    // audio routed from other tracks (always stereo)
    if (ch == 1) {
        for (int i = 0; i < frames; ++i) x[0][i] = (in[0][i] + in[1][i]) * 0.5f;
    } else {
        std::memcpy(x[0], in[0].data(), static_cast<std::size_t>(frames) * sizeof(float));
        std::memcpy(x[1], in[1].data(), static_cast<std::size_t>(frames) * sizeof(float));
    }

    // device input
    if (s.input >= 0 && inputs && inputs[s.input]) {
        const int pc = plan.inputs[static_cast<std::size_t>(s.input)]->channels;
        const float* src = inputs[s.input];
        if (s.inputChannel < pc)
            for (int c = 0; c < ch; ++c) {
                const int from = std::min(s.inputChannel + c, pc - 1);
                for (int i = 0; i < frames; ++i) x[c][i] += src[i * pc + from];
            }
    }

    st.lowcut.process(x, ch, frames, s.lowcut, s.lowcutHz);
    st.highcut.process(x, ch, frames, s.highcut, s.highcutHz);

    if (shm && s.shmTap == Tap::Pre) writeShm(shm, s.id, x, ch, frames);
    if (s.cue)
        for (int c = 0; c < 2; ++c) addScaled(cue_[c].data(), x[std::min(c, ch - 1)], frames);

    bool anyPre = false;
    for (const auto& send : s.sends) anyPre = anyPre || send.preFader;

    st.gain.setTarget(s.gain);
    const auto pg = panGains(s.pan, ch == 2);
    st.panLeft.setTarget(pg.left);
    st.panRight.setTarget(pg.right);
    float* post[2] = {post_[0].data(), post_[1].data()};
    float* pre[2] = {pre_[0].data(), pre_[1].data()};
    const float* xr = x[ch - 1];
    for (int i = 0; i < frames; ++i) {
        const float g = st.gain.next();
        const float l = st.panLeft.next();
        const float r = st.panRight.next();
        const float left = x[0][i] * l;
        const float right = xr[i] * r;
        if (anyPre) {
            pre[0][i] = left;
            pre[1][i] = right;
        }
        post[0][i] = left * g;
        post[1][i] = right * g;
        // the meter shows the strip after the fader, before the pan
        x[0][i] *= g;
        if (ch == 2) x[1][i] *= g;
    }
    meters_.add(s.id, x, ch, frames);

    for (const auto& d : s.outputs)
        for (int c = 0; c < 2; ++c) addScaled(destination(d, c), post[c], frames);

    for (const auto& send : s.sends) {
        auto& sg = st.sends[static_cast<std::size_t>(send.slot)];
        sg.setTarget(send.gain);
        float* const* src = send.preFader ? pre : post;
        float* dl = destination(send.dest, 0);
        float* dr = destination(send.dest, 1);
        for (int i = 0; i < frames; ++i) {
            const float g = sg.next();
            dl[i] += src[0][i] * g;
            dr[i] += src[1][i] * g;
        }
    }

    if (shm && s.shmTap == Tap::Post) writeShm(shm, s.id, post, 2, frames);
}

} // namespace dc::mixd
