#include "builtin.hpp"

#include "../dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dc::mixd {

namespace {

constexpr int kChunk = 32; // coefficient updates while a knob moves happen every 32 samples

inline float dbToGain(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }
inline double gainToDb(double g) { return 20.0 * std::log10(std::max(g, 1e-12)); }
// one-pole coefficient for a time constant in ms at 48 kHz, per sample
inline double coefMs(double ms) { return std::exp(-1.0 / (std::max(ms, 0.01) * 0.001 * kSampleRate)); }

// A parameter that glides towards its target once per chunk (~20 ms), linearly or in the log domain.
class Glide {
public:
    void reset(double v) {
        cur_ = target_ = v;
        init_ = true;
    }
    // the first target is taken at once (no glide from 0 when the effect starts)
    void setTarget(double v) {
        if (!init_) reset(v);
        target_ = v;
    }
    bool settled() const { return cur_ == target_; }
    double value() const { return cur_; }
    // returns true when the value changed
    bool step(bool logDomain) {
        if (cur_ == target_) return false;
        constexpr double k = 0.033; // per 32-sample chunk: ~20 ms time constant
        if (logDomain && cur_ > 0 && target_ > 0) cur_ *= std::pow(target_ / cur_, k);
        else cur_ += (target_ - cur_) * k;
        if (std::fabs(target_ - cur_) <= std::max(1e-4, std::fabs(target_) * 1e-4)) cur_ = target_;
        return true;
    }

private:
    double cur_ = 0, target_ = 0;
    bool init_ = false;
};

// Generic transposed direct form II biquad with RBJ cookbook designs.
class Biquad2 {
public:
    void setPeaking(double f, double gainDb, double q) {
        const double a = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * clampF(f) / kSampleRate;
        const double cs = std::cos(w), alpha = std::sin(w) / (2 * q);
        set(1 + alpha * a, -2 * cs, 1 - alpha * a, 1 + alpha / a, -2 * cs, 1 - alpha / a);
    }
    void setShelf(bool high, double f, double gainDb) {
        const double a = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * clampF(f) / kSampleRate;
        const double cs = std::cos(w), alpha = std::sin(w) / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(a) * alpha;
        if (!high)
            set(a * ((a + 1) - (a - 1) * cs + sa), 2 * a * ((a - 1) - (a + 1) * cs), a * ((a + 1) - (a - 1) * cs - sa),
                (a + 1) + (a - 1) * cs + sa, -2 * ((a - 1) + (a + 1) * cs), (a + 1) + (a - 1) * cs - sa);
        else
            set(a * ((a + 1) + (a - 1) * cs + sa), -2 * a * ((a - 1) + (a + 1) * cs), a * ((a + 1) + (a - 1) * cs - sa),
                (a + 1) - (a - 1) * cs + sa, 2 * ((a - 1) - (a + 1) * cs), (a + 1) - (a - 1) * cs - sa);
    }
    void reset() { z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0; }
    void process(float* x, int frames, int ch) {
        double z1 = z1_[ch], z2 = z2_[ch];
        for (int i = 0; i < frames; ++i) {
            const double in = x[i];
            const double y = b0_ * in + z1;
            z1 = b1_ * in - a1_ * y + z2;
            z2 = b2_ * in - a2_ * y;
            x[i] = static_cast<float>(y);
        }
        z1_[ch] = z1;
        z2_[ch] = z2;
    }

private:
    static double clampF(double f) { return std::clamp(f, 10.0, kSampleRate * 0.49); }
    void set(double b0, double b1, double b2, double a0, double a1, double a2) {
        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = a1 / a0;
        a2_ = a2 / a0;
    }
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double z1_[2] = {0, 0}, z2_[2] = {0, 0};
};

// ---------------------------------------------------------------------------------------------
// eq4: low shelf, two peaks, high shelf, output gain

class Eq4 final : public BuiltinEffect {
public:
    explicit Eq4(const proto::FxTypeSpec& s) : BuiltinEffect(s) {
        for (std::size_t i = 0; i < kParams; ++i) g_[i].reset(param(i));
        design();
    }

    void reset() override {
        for (auto& b : bands_) b.reset();
    }

    void process(float* const* x, int channels, int frames) override {
        if (paramsChanged())
            for (std::size_t i = 0; i < kParams; ++i) g_[i].setTarget(param(i));
        for (int off = 0; off < frames; off += kChunk) {
            const int n = std::min(kChunk, frames - off);
            bool moved = false;
            for (std::size_t i = 0; i < kParams; ++i) moved = g_[i].step(isFreqOrQ(i)) || moved;
            if (moved) design();
            for (int b = 0; b < 4; ++b) {
                if (gain(b) == 0.0 && g_[gainIndex(b)].settled()) continue; // flat band: exact identity, skip
                for (int c = 0; c < channels; ++c) bands_[b].process(x[c] + off, n, c);
            }
            const float out = dbToGain(g_[kOutput].value());
            if (out != 1.0f)
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < n; ++i) x[c][off + i] *= out;
        }
    }

private:
    // catalog order: low_freq, low_gain, mid1_freq, mid1_gain, mid1_q, mid2_freq, mid2_gain, mid2_q,
    // high_freq, high_gain, output
    static constexpr std::size_t kParams = 11, kOutput = 10;
    static bool isFreqOrQ(std::size_t i) { return i == 0 || i == 2 || i == 4 || i == 5 || i == 7 || i == 8; }
    static std::size_t gainIndex(int band) { return band == 0 ? 1 : band == 1 ? 3 : band == 2 ? 6 : 9; }
    double gain(int band) const { return g_[gainIndex(band)].value(); }
    void design() {
        bands_[0].setShelf(false, g_[0].value(), g_[1].value());
        bands_[1].setPeaking(g_[2].value(), g_[3].value(), g_[4].value());
        bands_[2].setPeaking(g_[5].value(), g_[6].value(), g_[7].value());
        bands_[3].setShelf(true, g_[8].value(), g_[9].value());
    }

    std::array<Glide, kParams> g_;
    std::array<Biquad2, 4> bands_;
};

// ---------------------------------------------------------------------------------------------
// comp: feed-forward, stereo-linked peak detector, soft knee, attack/release on the gain reduction

class Compressor final : public BuiltinEffect {
public:
    explicit Compressor(const proto::FxTypeSpec& s) : BuiltinEffect(s) {}

    void reset() override { env_ = 0; }
    float gainReductionDb() const override { return gr_.load(std::memory_order_relaxed); }

    void process(float* const* x, int channels, int frames) override {
        if (paramsChanged()) {
            thr_ = param(0);
            ratio_ = std::max(1.0f, param(1));
            att_ = coefMs(param(2));
            rel_ = coefMs(param(3));
            knee_ = param(4);
            makeup_.setTarget(param(5));
        }
        const double slope = 1.0 / ratio_ - 1.0;
        double maxGr = 0;
        for (int off = 0; off < frames; off += kChunk) {
            const int n = std::min(kChunk, frames - off);
            makeup_.step(false);
            const double makeup = makeup_.value();
            for (int i = off; i < off + n; ++i) {
                float peak = std::fabs(x[0][i]);
                if (channels > 1) peak = std::max(peak, std::fabs(x[1][i]));
                const double over = gainToDb(peak) - thr_;
                double gr; // target reduction, <= 0
                if (2 * over < -knee_) gr = 0;
                else if (knee_ > 0 && 2 * std::fabs(over) <= knee_) gr = slope * (over + knee_ / 2) * (over + knee_ / 2) / (2 * knee_);
                else gr = slope * over;
                env_ = gr < env_ ? gr + (env_ - gr) * att_ : gr + (env_ - gr) * rel_;
                maxGr = std::min(maxGr, env_);
                const float g = dbToGain(env_ + makeup);
                for (int c = 0; c < channels; ++c) x[c][i] *= g;
            }
        }
        gr_.store(static_cast<float>(maxGr), std::memory_order_relaxed);
    }

private:
    double thr_ = -18, ratio_ = 4, att_ = 0, rel_ = 0, knee_ = 6;
    Glide makeup_;
    double env_ = 0; // smoothed gain reduction dB
    std::atomic<float> gr_{0};
};

// ---------------------------------------------------------------------------------------------
// gate: peak detector with hysteresis (4 dB) and hold; opens with attack, closes with release

class Gate final : public BuiltinEffect {
public:
    explicit Gate(const proto::FxTypeSpec& s) : BuiltinEffect(s) {}

    void reset() override {
        det_ = 0;
        gain_ = 1;
        open_ = true;
        hold_ = 0;
    }
    float gainReductionDb() const override { return gr_.load(std::memory_order_relaxed); }

    void process(float* const* x, int channels, int frames) override {
        if (paramsChanged()) {
            openLin_ = dbToGain(param(0));
            closeLin_ = dbToGain(param(0) - 4.0);
            floor_ = param(1) <= -90.0f ? 0.0f : dbToGain(param(1));
            att_ = coefMs(param(2));
            holdSamples_ = static_cast<int>(param(3) * 0.001 * kSampleRate);
            rel_ = coefMs(param(4));
        }
        constexpr double detRelease = 0.99896; // 20 ms detector decay
        float minGain = 1;
        for (int i = 0; i < frames; ++i) {
            float peak = std::fabs(x[0][i]);
            if (channels > 1) peak = std::max(peak, std::fabs(x[1][i]));
            det_ = std::max<double>(peak, det_ * detRelease);
            if (det_ >= openLin_) {
                open_ = true;
                hold_ = holdSamples_;
            } else if (det_ < closeLin_) {
                if (hold_ > 0) --hold_;
                else open_ = false;
            }
            const double target = open_ ? 1.0 : floor_;
            gain_ = target > gain_ ? target + (gain_ - target) * att_ : target + (gain_ - target) * rel_;
            minGain = std::min(minGain, static_cast<float>(gain_));
            for (int c = 0; c < channels; ++c) x[c][i] *= static_cast<float>(gain_);
        }
        gr_.store(static_cast<float>(gainToDb(minGain)), std::memory_order_relaxed);
    }

private:
    double openLin_ = 0, closeLin_ = 0, floor_ = 0, att_ = 0, rel_ = 0;
    int holdSamples_ = 0;
    double det_ = 0, gain_ = 1;
    bool open_ = true;
    int hold_ = 0;
    std::atomic<float> gr_{0};
};

// ---------------------------------------------------------------------------------------------
// reverb: Freeverb (Jezar, public domain) at 48 kHz with a pre-delay

class Reverb final : public BuiltinEffect {
public:
    explicit Reverb(const proto::FxTypeSpec& s) : BuiltinEffect(s) {
        constexpr int combs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        constexpr int allpasses[4] = {556, 441, 341, 225};
        constexpr int spread = 23;
        const double scale = kSampleRate / 44100.0;
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 8; ++k) comb_[c][k].buf.assign(static_cast<std::size_t>((combs[k] + c * spread) * scale), 0.0f);
            for (int k = 0; k < 4; ++k)
                allpass_[c][k].buf.assign(static_cast<std::size_t>((allpasses[k] + c * spread) * scale), 0.0f);
        }
        pre_.assign(static_cast<std::size_t>(0.2 * kSampleRate) + 1, 0.0f);
    }

    void reset() override {
        for (auto& side : comb_)
            for (auto& cf : side) {
                std::fill(cf.buf.begin(), cf.buf.end(), 0.0f);
                cf.store = 0;
            }
        for (auto& side : allpass_)
            for (auto& ap : side) std::fill(ap.buf.begin(), ap.buf.end(), 0.0f);
        std::fill(pre_.begin(), pre_.end(), 0.0f);
    }

    void process(float* const* x, int channels, int frames) override {
        if (paramsChanged()) {
            size_.setTarget(param(0));
            damp_.setTarget(param(1));
            preDelay_ = std::clamp(static_cast<int>(param(2) * 0.001 * kSampleRate), 0, static_cast<int>(pre_.size()) - 1);
            width_.setTarget(param(3));
            mix_.setTarget(param(4));
        }
        for (int off = 0; off < frames; off += kChunk) {
            const int n = std::min(kChunk, frames - off);
            size_.step(false);
            damp_.step(false);
            width_.step(false);
            mix_.step(false);
            const float feedback = static_cast<float>(size_.value() * 0.28 + 0.7);
            const float damp1 = static_cast<float>(damp_.value() * 0.4), damp2 = 1 - damp1;
            const float mix = static_cast<float>(mix_.value());
            const float wet = 3.0f * mix; // Freeverb's wet scale
            const float wet1 = wet * static_cast<float>(width_.value() / 2 + 0.5);
            const float wet2 = wet * static_cast<float>((1 - width_.value()) / 2);
            for (int i = off; i < off + n; ++i) {
                const float in = channels > 1 ? x[0][i] + x[1][i] : 2 * x[0][i];
                pre_[preWrite_] = in * 0.015f;
                std::size_t rd = preWrite_ + pre_.size() - static_cast<std::size_t>(preDelay_);
                if (rd >= pre_.size()) rd -= pre_.size();
                const float input = pre_[rd];
                if (++preWrite_ == pre_.size()) preWrite_ = 0;

                float out[2] = {0, 0};
                for (int c = 0; c < 2; ++c) {
                    for (auto& cf : comb_[c]) {
                        const float y = cf.buf[cf.pos];
                        cf.store = y * damp2 + cf.store * damp1;
                        cf.buf[cf.pos] = input + cf.store * feedback;
                        if (++cf.pos == cf.buf.size()) cf.pos = 0;
                        out[c] += y;
                    }
                    for (auto& ap : allpass_[c]) {
                        const float b = ap.buf[ap.pos];
                        ap.buf[ap.pos] = out[c] + b * 0.5f;
                        if (++ap.pos == ap.buf.size()) ap.pos = 0;
                        out[c] = b - out[c];
                    }
                }
                if (channels > 1) {
                    const float l = out[0] * wet1 + out[1] * wet2, r = out[1] * wet1 + out[0] * wet2;
                    x[0][i] = x[0][i] * (1 - mix) + l;
                    x[1][i] = x[1][i] * (1 - mix) + r;
                } else {
                    x[0][i] = x[0][i] * (1 - mix) + (out[0] + out[1]) * 0.5f * wet;
                }
            }
        }
    }

private:
    struct Comb {
        std::vector<float> buf;
        std::size_t pos = 0;
        float store = 0;
    };
    struct Allpass {
        std::vector<float> buf;
        std::size_t pos = 0;
    };
    std::array<std::array<Comb, 8>, 2> comb_;
    std::array<std::array<Allpass, 4>, 2> allpass_;
    std::vector<float> pre_;
    std::size_t preWrite_ = 0;
    int preDelay_ = 0;
    Glide size_, damp_, width_, mix_;
};

// ---------------------------------------------------------------------------------------------
// delay: up to 2 s, smoothed time (no clicks, a short pitch glide), low-passed feedback, ping-pong

class Delay final : public BuiltinEffect {
public:
    explicit Delay(const proto::FxTypeSpec& s) : BuiltinEffect(s) {
        for (auto& b : buf_) b.assign(kSize, 0.0f);
        time_ = param(0) * 0.001 * kSampleRate;
    }

    void reset() override {
        for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0f);
        lp_[0] = lp_[1] = 0;
        time_ = param(0) * 0.001 * kSampleRate;
    }

    void process(float* const* x, int channels, int frames) override {
        if (paramsChanged()) {
            timeTarget_ = std::clamp(param(0) * 0.001 * kSampleRate, 1.0, kSize - 4.0);
            feedback_.setTarget(std::clamp(param(1), 0.0f, 0.95f));
            lpCoef_ = 1.0 - std::exp(-2 * kPi * param(2) / kSampleRate);
            pingPong_ = param(3) >= 0.5f;
            mix_.setTarget(param(4));
        }
        constexpr double timeCoef = 0.99958; // ~50 ms glide of the delay time
        for (int off = 0; off < frames; off += kChunk) {
            const int n = std::min(kChunk, frames - off);
            feedback_.step(false);
            mix_.step(false);
            const float fb = static_cast<float>(feedback_.value()), mix = static_cast<float>(mix_.value());
            for (int i = off; i < off + n; ++i) {
                time_ = timeTarget_ + (time_ - timeTarget_) * timeCoef;
                float wet[2];
                const int lines = channels > 1 ? 2 : 1;
                for (int c = 0; c < lines; ++c) wet[c] = read(c);
                for (int c = 0; c < lines; ++c) lp_[c] += (wet[c] - lp_[c]) * lpCoef_;
                if (lines == 2 && pingPong_) {
                    buf_[0][write_] = (x[0][i] + x[1][i]) * 0.5f + static_cast<float>(lp_[1]) * fb;
                    buf_[1][write_] = static_cast<float>(lp_[0]) * fb;
                } else {
                    for (int c = 0; c < lines; ++c) buf_[c][write_] = x[c][i] + static_cast<float>(lp_[c]) * fb;
                }
                for (int c = 0; c < lines; ++c) x[c][i] = x[c][i] * (1 - mix) + wet[c] * mix;
                write_ = (write_ + 1) & (kSize - 1);
            }
        }
    }

private:
    static constexpr std::size_t kSize = 131072; // > 2 s at 48 kHz, power of two
    float read(int c) const {
        const double pos = static_cast<double>(write_) + kSize - time_;
        const auto i0 = static_cast<std::size_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(i0));
        const float a = buf_[c][i0 & (kSize - 1)], b = buf_[c][(i0 + 1) & (kSize - 1)];
        return a + (b - a) * frac;
    }
    std::array<std::vector<float>, 2> buf_;
    std::size_t write_ = 0;
    double time_ = 0, timeTarget_ = 14400;
    double lp_[2] = {0, 0}, lpCoef_ = 0.65;
    bool pingPong_ = false;
    Glide feedback_, mix_;
};

// ---------------------------------------------------------------------------------------------
// limiter: brickwall with 1 ms look-ahead. Hold the minimum required gain over the window, release,
// then a box filter of the same length; the audio is delayed so the gain is always low enough.

class Limiter final : public BuiltinEffect {
public:
    explicit Limiter(const proto::FxTypeSpec& s) : BuiltinEffect(s) { reset(); }

    void reset() override {
        for (auto& d : delay_) d.fill(0.0f);
        req_.fill(1.0f);
        box_.fill(1.0f);
        boxSum_ = kWindow;
        env_ = 1;
        pos_ = 0;
    }
    float gainReductionDb() const override { return gr_.load(std::memory_order_relaxed); }

    void process(float* const* x, int channels, int frames) override {
        if (paramsChanged()) {
            input_.setTarget(dbToGain(param(0)));
            ceiling_ = dbToGain(param(1));
            rel_ = coefMs(param(2));
        }
        float minGain = 1;
        for (int off = 0; off < frames; off += kChunk) {
            const int n = std::min(kChunk, frames - off);
            input_.step(false);
            const float in = static_cast<float>(input_.value());
            for (int i = off; i < off + n; ++i) {
                float s[2] = {x[0][i] * in, channels > 1 ? x[1][i] * in : 0.0f};
                const float peak = std::max(std::fabs(s[0]), std::fabs(s[1]));
                const std::size_t p = pos_;
                req_[p] = peak > ceiling_ ? ceiling_ / peak : 1.0f;
                float held = 1.0f;
                for (float r : req_) held = std::min(held, r);
                env_ = held < env_ ? held : held + (env_ - held) * rel_;
                boxSum_ += env_ - box_[p];
                box_[p] = static_cast<float>(env_);
                // the box sum drifts with rounding: recompute it once per window
                if (p == 0) {
                    boxSum_ = 0;
                    for (float b : box_) boxSum_ += b;
                }
                const float gain = static_cast<float>(boxSum_ / kWindow);
                minGain = std::min(minGain, gain);
                // output the sample from kWindow - 1 samples ago
                const std::size_t rd = (p + 1) % kWindow;
                for (int c = 0; c < channels; ++c) {
                    const float delayed = delay_[c][rd];
                    delay_[c][p] = s[c];
                    x[c][i] = std::clamp(delayed * gain, -ceiling_, ceiling_);
                }
                pos_ = (p + 1) % kWindow;
            }
        }
        gr_.store(static_cast<float>(gainToDb(minGain)), std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kWindow = 49; // latency kWindow - 1 = 48 samples (catalog)
    std::array<std::array<float, kWindow>, 2> delay_{};
    std::array<float, kWindow> req_{}, box_{};
    double boxSum_ = kWindow, env_ = 1, rel_ = 0;
    float ceiling_ = 0.89f;
    std::size_t pos_ = 0;
    Glide input_;
    std::atomic<float> gr_{0};
};

} // namespace

BuiltinEffect::BuiltinEffect(const proto::FxTypeSpec& spec)
    : spec_(spec), values_(std::make_unique<std::atomic<float>[]>(spec.params.size())) {
    for (std::size_t i = 0; i < spec.params.size(); ++i) values_[i].store(static_cast<float>(spec.params[i].defaultValue));
}

void BuiltinEffect::setParam(std::string_view key, double value) {
    for (std::size_t i = 0; i < spec_.params.size(); ++i) {
        const auto& p = spec_.params[i];
        if (p.key != key) continue;
        values_[i].store(static_cast<float>(std::clamp(value, p.min, p.max)), std::memory_order_relaxed);
        changed_.store(true, std::memory_order_release);
        return;
    }
}

std::shared_ptr<BuiltinEffect> makeBuiltinEffect(std::string_view type) {
    const auto* spec = proto::findBuiltinFx(type);
    if (!spec) return nullptr;
    if (type == "eq4") return std::make_shared<Eq4>(*spec);
    if (type == "comp") return std::make_shared<Compressor>(*spec);
    if (type == "gate") return std::make_shared<Gate>(*spec);
    if (type == "reverb") return std::make_shared<Reverb>(*spec);
    if (type == "delay") return std::make_shared<Delay>(*spec);
    if (type == "limiter") return std::make_shared<Limiter>(*spec);
    return nullptr;
}

// --- FxSlot ---

FxSlot::FxSlot(std::shared_ptr<Effect> effect, bool bypass)
    : effect_(std::move(effect)), bypass_(bypass), wet_(bypass ? 0.0f : 1.0f) {}

void FxSlot::process(float* const* x, int channels, int frames) {
    const float target = bypass_.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
    if (wet_ == 0 && target == 0) return;
    if (wet_ == 1 && target == 1) {
        effect_->process(x, channels, frames);
        return;
    }
    if (wet_ == 0) effect_->reset(); // coming back: no stale tail from before the bypass
    for (int c = 0; c < channels; ++c) std::memcpy(dry_[c].data(), x[c], static_cast<std::size_t>(frames) * sizeof(float));
    effect_->process(x, channels, frames);
    constexpr float kStep = 1.0f / 480.0f; // 10 ms
    float w = wet_;
    for (int i = 0; i < frames; ++i) {
        w = target > w ? std::min(target, w + kStep) : std::max(target, w - kStep);
        for (int c = 0; c < channels; ++c) x[c][i] = dry_[c][i] + (x[c][i] - dry_[c][i]) * w;
    }
    wet_ = w;
}

} // namespace dc::mixd
