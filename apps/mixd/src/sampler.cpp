#include "sampler.hpp"

#include "audio_file.hpp"

#include <algorithm>
#include <cstring>

namespace dc::mixd {

PadMode padModeFromString(const std::string& s) {
    if (s == "toggle") return PadMode::Toggle;
    if (s == "hold") return PadMode::Hold;
    if (s == "loop") return PadMode::Loop;
    return PadMode::OneShot;
}

Sampler::Sampler() : buses_(std::make_unique<std::array<std::array<Buffer, 2>, kSamplerBanks + 1>>()) {
    for (auto& b : *buses_)
        for (auto& c : b) c.fill(0);
}

void Sampler::adopt(const SamplerTable* table) {
    table_ = table;
    for (auto& v : voices_) {
        if (!v.active) continue;
        // the old plan (and with it the sample) may be freed once this plan is adopted
        if (!table_ || table_->pads[static_cast<std::size_t>(v.pad)].sample.get() != v.sample) v.active = false;
    }
}

Sampler::Voice* Sampler::sounding(int pad) {
    for (auto& v : voices_)
        if (v.active && v.pad == pad && v.fade == 0) return &v;
    return nullptr;
}

void Sampler::fadeOut(Voice& v) {
    if (v.fade == 0) v.fade = kSamplerFadeFrames;
}

void Sampler::start(int pad) {
    const auto& p = table_->pads[static_cast<std::size_t>(pad)];
    Voice* slot = nullptr;
    for (auto& v : voices_)
        if (!v.active) {
            slot = &v;
            break;
        }
    if (!slot) {
        // all busy: take the oldest, preferring voices that are already fading out
        for (auto& v : voices_)
            if (!slot || (v.fade > 0) > (slot->fade > 0) ||
                ((v.fade > 0) == (slot->fade > 0) && v.started < slot->started))
                slot = &v;
    }
    *slot = Voice{};
    slot->active = true;
    slot->pad = pad;
    slot->sample = p.sample.get();
    slot->loop = p.mode == PadMode::Loop;
    slot->hold = p.mode == PadMode::Hold;
    slot->gain = p.gain;
    slot->started = ++counter_;
}

void Sampler::handle(const Command& c) {
    switch (c.type) {
        case Command::Type::Trigger: {
            if (!table_ || c.index < 0 || c.index >= kSamplerPadCount) return;
            const auto& p = table_->pads[static_cast<std::size_t>(c.index)];
            if (!p.sample || p.sample->frames == 0) return;
            Voice* v = sounding(c.index);
            if (v) fadeOut(*v);
            const bool toggles = p.mode == PadMode::Toggle || p.mode == PadMode::Loop;
            if (!(toggles && v)) start(c.index);
            break;
        }
        case Command::Type::Release:
            if (Voice* v = sounding(c.index); v && v->hold) fadeOut(*v);
            break;
        case Command::Type::StopAll:
            for (auto& v : voices_)
                if (v.active) fadeOut(v);
            break;
        case Command::Type::StopBank:
            for (auto& v : voices_)
                if (v.active && v.pad / kSamplerPadsPerBank == c.index - 1) fadeOut(v);
            break;
    }
}

void Sampler::render(int frames) {
    Command c;
    while (queue_.pop(c)) handle(c);

    const auto bytes = static_cast<std::size_t>(frames) * sizeof(float);
    for (auto& b : *buses_)
        for (auto& ch : b) std::memset(ch.data(), 0, bytes);

    std::uint64_t bits[2] = {0, 0};
    for (auto& v : voices_) {
        if (!v.active) continue;
        const float target = table_ ? table_->pads[static_cast<std::size_t>(v.pad)].gain : 0.0f;
        const float g0 = v.gain;
        const float gStep = (target - g0) / static_cast<float>(frames); // volume changes glide over the block
        auto& bus = (*buses_)[static_cast<std::size_t>(v.pad / kSamplerPadsPerBank + 1)];
        const float* src = v.sample->samples.data();
        const std::size_t len = v.sample->frames;
        for (int i = 0; i < frames; ++i) {
            if (v.pos >= len) {
                if (!v.loop) {
                    v.active = false;
                    break;
                }
                v.pos = 0;
            }
            float g = g0 + gStep * static_cast<float>(i + 1);
            if (v.fade > 0) {
                g *= static_cast<float>(v.fade) / kSamplerFadeFrames;
                if (--v.fade == 0) {
                    v.active = false;
                    break;
                }
            }
            bus[0][static_cast<std::size_t>(i)] += src[2 * v.pos] * g;
            bus[1][static_cast<std::size_t>(i)] += src[2 * v.pos + 1] * g;
            ++v.pos;
        }
        v.gain = target;
        if (v.active) bits[v.pad / 64] |= 1ull << (v.pad % 64);
    }

    auto& all = (*buses_)[0];
    for (int b = 1; b <= kSamplerBanks; ++b)
        for (int ch = 0; ch < 2; ++ch) {
            const float* s = (*buses_)[static_cast<std::size_t>(b)][static_cast<std::size_t>(ch)].data();
            float* d = all[static_cast<std::size_t>(ch)].data();
            for (int i = 0; i < frames; ++i) d[i] += s[i];
        }
    playing_[0].store(bits[0], std::memory_order_relaxed);
    playing_[1].store(bits[1], std::memory_order_relaxed);
}

// --- loader ---

SampleLoader::SampleLoader(Callback callback) : callback_(std::move(callback)), thread_([this] { run(); }) {}

SampleLoader::~SampleLoader() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    thread_.join();
}

void SampleLoader::load(int pad, std::string file, std::uint64_t serial) {
    {
        std::lock_guard lock(mutex_);
        // a newer request for the same pad replaces one that has not started yet
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Request& r) { return r.pad == pad; }),
                     queue_.end());
        queue_.push_back({pad, std::move(file), serial});
    }
    cv_.notify_one();
}

void SampleLoader::run() {
    for (;;) {
        Request req;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            req = std::move(queue_.front());
            queue_.pop_front();
        }
        Result r;
        r.pad = req.pad;
        r.file = req.file;
        r.serial = req.serial;
        DecodedAudio audio;
        if (decodeAudioFile(pathFromUtf8(req.file), audio, &r.error)) {
            auto data = std::make_shared<SampleData>();
            data->samples = std::move(audio.samples);
            data->frames = audio.frames;
            r.sample = std::move(data);
        }
        callback_(std::move(r));
    }
}

} // namespace dc::mixd
