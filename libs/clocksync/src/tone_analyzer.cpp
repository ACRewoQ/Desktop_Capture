#include "dc/clocksync/tone_analyzer.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace dc::clocksync {

namespace {

constexpr double kPi = 3.14159265358979323846;

double wrapPi(double a) { return a - 2 * kPi * std::floor((a + kPi) / (2 * kPi)); }

// Solves the normal equations M x = v (n <= 4) by Gaussian elimination with partial pivoting.
template <std::size_t N> bool solve(std::array<std::array<double, N>, N> m, std::array<double, N> v,
                                    std::array<double, N>& x) {
    for (std::size_t c = 0; c < N; ++c) {
        std::size_t p = c;
        for (std::size_t r = c + 1; r < N; ++r)
            if (std::abs(m[r][c]) > std::abs(m[p][c])) p = r;
        if (std::abs(m[p][c]) < 1e-300) return false;
        std::swap(m[p], m[c]);
        std::swap(v[p], v[c]);
        for (std::size_t r = c + 1; r < N; ++r) {
            const double f = m[r][c] / m[c][c];
            for (std::size_t k = c; k < N; ++k) m[r][k] -= f * m[c][k];
            v[r] -= f * v[c];
        }
    }
    for (std::size_t c = N; c-- > 0;) {
        double s = v[c];
        for (std::size_t k = c + 1; k < N; ++k) s -= m[c][k] * x[k];
        x[c] = s / m[c][c];
    }
    return true;
}

} // namespace

ToneAnalyzer::ToneAnalyzer(Config config) : cfg_(config) {
    nominalOmega_ = 2 * kPi * cfg_.toneHz / cfg_.sampleRate;
    omega_ = nominalOmega_;
    buf_.reserve(cfg_.blockFrames);
}

ToneAnalyzer::Block ToneAnalyzer::analyzeBlock() {
    const std::size_t n = buf_.size();
    const double mid = double(n - 1) / 2;
    double a = 0, b = 0, c = 0;
    double w = omega_;

    double peak = 0, energy = 0;
    for (float s : buf_) {
        peak = std::max(peak, double(std::abs(s)));
        energy += double(s) * s;
    }

    // Four-parameter fit around the centre of the block: x = a cos(w m) + b sin(w m) + c.
    // Iterations: 3-parameter linear solve, then Gauss-Newton steps that also refine w.
    bool ok = peak >= cfg_.minAmplitude;
    for (int it = 0; ok && it < 5; ++it) {
        std::array<std::array<double, 4>, 4> m{};
        std::array<double, 4> v{};
        const std::size_t dim = it == 0 ? 3 : 4;
        // cos/sin by rotation: exact enough over one block in double precision
        const double cw = std::cos(w), sw = std::sin(w);
        double co = std::cos(-w * mid), si = std::sin(-w * mid);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = double(i) - mid;
            const double col[4] = {co, si, 1.0, -a * t * si + b * t * co};
            const double x = buf_[i];
            for (std::size_t r = 0; r < dim; ++r) {
                v[r] += col[r] * x;
                for (std::size_t k = r; k < dim; ++k) m[r][k] += col[r] * col[k];
            }
            const double nco = co * cw - si * sw;
            si = si * cw + co * sw;
            co = nco;
        }
        for (std::size_t r = 0; r < dim; ++r)
            for (std::size_t k = 0; k < r; ++k) m[r][k] = m[k][r];
        if (dim == 3) {
            std::array<std::array<double, 3>, 3> m3{};
            std::array<double, 3> v3{}, x3{};
            for (std::size_t r = 0; r < 3; ++r) {
                v3[r] = v[r];
                for (std::size_t k = 0; k < 3; ++k) m3[r][k] = m[r][k];
            }
            ok = solve(m3, v3, x3);
            a = x3[0], b = x3[1], c = x3[2];
        } else {
            std::array<double, 4> x4{};
            ok = solve(m, v, x4);
            a = x4[0], b = x4[1], c = x4[2];
            const double dw = std::clamp(x4[3], -0.01 * nominalOmega_, 0.01 * nominalOmega_);
            w += dw;
            if (std::abs(dw) < 1e-13) break;
        }
    }

    Block blk;
    blk.index = next_ / cfg_.blockFrames;
    blk.startFrame = next_;
    if (ok && std::abs(w - nominalOmega_) < 0.02 * nominalOmega_) {
        double residual = 0;
        const double cw = std::cos(w), sw = std::sin(w);
        double co = std::cos(-w * mid), si = std::sin(-w * mid);
        for (std::size_t i = 0; i < n; ++i) {
            const double e = buf_[i] - (a * co + b * si + c);
            residual += e * e;
            const double nco = co * cw - si * sw;
            si = si * cw + co * sw;
            co = nco;
        }
        const double amp = std::sqrt(a * a + b * b);
        const double signal = amp * amp / 2 * double(n);
        blk.amplitude = amp;
        blk.frequencyHz = w * cfg_.sampleRate / (2 * kPi);
        blk.sinadDb = 10 * std::log10(signal / std::max(residual, 1e-30));
        // phase at the block start: a cos + b sin = amp cos(w m - atan2(b, a)), m = -mid
        const double startPhase = -w * mid - std::atan2(b, a);
        if (tracking_) {
            phase_ += wrapPi(startPhase - phase_);
        } else {
            phase_ = startPhase;
            tracking_ = true;
        }
        blk.unwrappedPhase = phase_;
        phase_ += w * double(n);
        omega_ = w;
    } else {
        blk.amplitude = peak;
        blk.sinadDb = energy > 0 ? -99 : -100;
        resetTracking();
    }
    blk.glitch = blk.sinadDb < cfg_.glitchSinadDb || blk.amplitude < cfg_.minAmplitude;

    ++summary_.blocks;
    sinadSum_ += blk.sinadDb;
    summary_.meanSinadDb = sinadSum_ / double(summary_.blocks);
    summary_.minSinadDb = std::min(summary_.minSinadDb, blk.sinadDb);
    if (blk.glitch) {
        ++summary_.glitchBlocks;
        if (summary_.events.size() < cfg_.maxEvents) summary_.events.push_back(blk);
    }
    next_ += n;
    buf_.clear();
    return blk;
}

} // namespace dc::clocksync
