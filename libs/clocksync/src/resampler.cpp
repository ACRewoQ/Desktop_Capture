#include "dc/clocksync/resampler.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace dc::clocksync {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Zeroth-order modified Bessel function of the first kind (series; converges fast for beta <= 20).
double besselI0(double x) {
    double sum = 1, term = 1;
    const double q = x * x / 4;
    for (int k = 1; k < 64; ++k) {
        term *= q / (double(k) * double(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

double kernel(double x, double cutoff, double halfTaps, double beta) {
    if (std::abs(x) >= halfTaps) return 0;
    const double a = kPi * cutoff * x;
    const double sinc = std::abs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
    const double r = x / halfTaps;
    const double window = besselI0(beta * std::sqrt(1 - r * r)) / besselI0(beta);
    return cutoff * sinc * window;
}

} // namespace

VariableResampler::VariableResampler(int channels, Quality quality, std::size_t maxBlockFrames)
    : channels_(channels), q_(quality) {
    assert(channels >= 1 && q_.halfTaps >= 2 && q_.phases >= 2);
    const int taps = 2 * q_.halfTaps;
    table_.resize(std::size_t(q_.phases + 1) * taps);
    for (int p = 0; p <= q_.phases; ++p) {
        const double frac = double(p) / q_.phases;
        float* row = &table_[std::size_t(p) * taps];
        double sum = 0;
        for (int j = 0; j < taps; ++j) {
            const double x = double(j - q_.halfTaps + 1) - frac;
            const double w = kernel(x, q_.cutoff, q_.halfTaps, q_.kaiserBeta);
            row[j] = float(w);
            sum += w;
        }
        // exact unity gain at DC for every phase, so the ratio wobble never modulates the level
        for (int j = 0; j < taps; ++j) row[j] = float(row[j] / sum);
    }
    // a block may consume up to ~1% more than it produces; keep room for history on both sides
    capacity_ = 2 * maxBlockFrames + 4 * std::size_t(q_.halfTaps) + 64;
    buf_.assign(capacity_ * channels_, 0.0f);
    reset();
}

void VariableResampler::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    end_ = std::size_t(q_.halfTaps);
    pos_ = double(q_.halfTaps);
}

std::size_t VariableResampler::inputFramesRequired(std::size_t outFrames, double ratio) const {
    if (outFrames == 0) return 0;
    const double last = pos_ + double(outFrames - 1) * ratio;
    const auto need = std::size_t(std::floor(last)) + std::size_t(q_.halfTaps) + 1;
    return need > end_ ? need - end_ : 0;
}

void VariableResampler::compact() {
    const double first = std::floor(pos_) - double(q_.halfTaps) + 1;
    if (first <= 0) return;
    const auto drop = std::size_t(first);
    std::memmove(buf_.data(), buf_.data() + drop * channels_, (end_ - drop) * channels_ * sizeof(float));
    end_ -= drop;
    pos_ -= double(drop);
}

float* VariableResampler::inputWritePointer(std::size_t frames) {
    if (end_ + frames > capacity_) compact();
    assert(end_ + frames <= capacity_);
    return buf_.data() + end_ * channels_;
}

void VariableResampler::commitInput(std::size_t frames) {
    assert(end_ + frames <= capacity_);
    end_ += frames;
}

void VariableResampler::produce(float* out, std::size_t outFrames, double ratio) {
    assert(inputFramesRequired(outFrames, ratio) == 0);
    const int taps = 2 * q_.halfTaps;
    const int ch = channels_;
    for (std::size_t i = 0; i < outFrames; ++i) {
        const double t = pos_ + double(i) * ratio;
        const double n = std::floor(t);
        const double fp = (t - n) * q_.phases;
        const int p = std::min(int(fp), q_.phases - 1);
        const float a = float(fp - p);
        const float* w0 = &table_[std::size_t(p) * taps];
        const float* w1 = w0 + taps;
        const float* x = buf_.data() + (std::size_t(n) - std::size_t(q_.halfTaps) + 1) * ch;
        float* y = out + i * ch;
        if (ch == 2) {
            float l = 0, r = 0;
            for (int j = 0; j < taps; ++j) {
                const float w = w0[j] + a * (w1[j] - w0[j]);
                l += x[2 * j] * w;
                r += x[2 * j + 1] * w;
            }
            y[0] = l;
            y[1] = r;
        } else {
            for (int c = 0; c < ch; ++c) {
                float s = 0;
                for (int j = 0; j < taps; ++j) s += x[j * ch + c] * (w0[j] + a * (w1[j] - w0[j]));
                y[c] = s;
            }
        }
    }
    pos_ += double(outFrames) * ratio;
}

} // namespace dc::clocksync
