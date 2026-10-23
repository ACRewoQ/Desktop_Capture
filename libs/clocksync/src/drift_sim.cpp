#include "dc/clocksync/drift_sim.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>
#include <vector>

namespace dc::clocksync {

namespace {

constexpr double kPi = 3.14159265358979323846;

class ClockModel {
public:
    ClockModel(const SimClock& c, double nominal) : c_(c), nominal_(nominal) {}

    double rateAt(double t) const {
        return nominal_ * (1 + 1e-6 * (c_.ppm + c_.wanderPpm * std::sin(2 * kPi * t / c_.wanderPeriodSec)));
    }
    double framesAt(double t) const {
        const double w = c_.wanderPeriodSec;
        return nominal_ *
               (t + 1e-6 * (c_.ppm * t + c_.wanderPpm * w / (2 * kPi) * (1 - std::cos(2 * kPi * t / w))));
    }
    double timeOf(double frame) const {
        double t = frame / nominal_;
        for (int i = 0; i < 4; ++i) t -= (framesAt(t) - frame) / rateAt(t);
        return t;
    }

private:
    SimClock c_;
    double nominal_;
};

} // namespace

bool SimResult::passed() const {
    return stats.underruns == 0 && stats.overruns == 0 && stats.resyncs == 0 && tone.glitchBlocks == 0 &&
           latencyDriftMs < 0.5;
}

std::string SimResult::verdict() const {
    std::ostringstream o;
    o << (passed() ? "PASS" : "FAIL") << "  underruns " << stats.underruns << ", overruns " << stats.overruns
      << ", resyncs " << stats.resyncs << ", glitch blocks " << tone.glitchBlocks << "/" << tone.blocks
      << ", min SINAD " << tone.minSinadDb << " dB, latency drift " << latencyDriftMs * 1000 << " us"
      << ", drift true " << truePpm << " ppm / estimated " << estimatedPpm << " ppm";
    return o.str();
}

SimResult runSimulation(const SimConfig& cfg, const std::function<void(const SimProgress&)>& progress) {
    const ClockModel prod(cfg.producer, cfg.nominalRate);
    const ClockModel cons(cfg.consumer, cfg.nominalRate);
    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::normal_distribution<double> gauss(0.0, 1.0);

    auto bridgeCfg = cfg.bridge;
    bridgeCfg.nominalRate = cfg.nominalRate;
    const int ch = bridgeCfg.channels;
    ClockBridge bridge(bridgeCfg);

    auto anaCfg = cfg.analyzer;
    anaCfg.sampleRate = cfg.nominalRate;
    anaCfg.toneHz = cfg.toneHz;
    ToneAnalyzer analyzer(anaCfg);
    const double omega0 = 2 * kPi * cfg.toneHz / cfg.nominalRate; // tone phase per producer frame

    const std::size_t pP = cfg.producer.period, pC = cfg.consumer.period;
    std::vector<float> inBuf(pP * ch), outBuf(pC * ch);
    std::uint64_t kP = 0, kC = 0;
    double nextP = prod.timeOf(double(pP)) + uni(rng) * cfg.producer.callbackJitterSec;
    double nextC = cons.timeOf(0) + uni(rng) * cfg.consumer.callbackJitterSec;

    SimResult res;
    bool analyzing = false, warm = false;
    std::uint64_t analyzerStartFrame = 0; // consumer frame index of the analyzer's frame 0
    double latMin = 1e9, latMax = -1e9, lastLatency = 0;
    double fillMin = 1e9, fillMax = -1e9, fifoMin = 1e9, fifoMax = -1e9;
    double prevRatio = 1, maxStep = 0;
    double nextProgress = cfg.progressEverySec;

    auto truePpmAt = [&](double t) { return (prod.rateAt(t) / cons.rateAt(t) - 1) * 1e6; };

    while (true) {
        if (nextP <= nextC) {
            // producer packet [kP, kP + pP) is complete
            for (std::size_t i = 0; i < pP; ++i) {
                const double cyc = std::fmod(double(kP + i) * cfg.toneHz / cfg.nominalRate, 1.0);
                const float s = float(cfg.amplitude * std::sin(2 * kPi * cyc));
                for (int c = 0; c < ch; ++c) inBuf[i * ch + c] = s;
            }
            const double stamp = prod.timeOf(double(kP)) + gauss(rng) * cfg.producer.timestampJitterSec;
            bridge.push(inBuf.data(), pP, stamp);
            kP += pP;
            nextP = prod.timeOf(double(kP + pP)) + uni(rng) * cfg.producer.callbackJitterSec;
            continue;
        }

        const double t = cons.timeOf(double(kC));
        if (t > cfg.seconds) break;
        const double now = t + gauss(rng) * cfg.consumer.timestampJitterSec;
        bridge.pull(outBuf.data(), pC, now);
        const auto st = bridge.stats();

        if (!analyzing && st.state == ClockBridge::State::Running) {
            analyzing = true;
            analyzerStartFrame = kC;
        }
        if (analyzing) {
            analyzer.feed(outBuf.data(), pC, ch, [&](const ToneAnalyzer::Block& b) {
                if (!warm) return;
                // the tone phase tells which producer frame is playing (up to whole tone periods)
                const double srcFrame = (b.unwrappedPhase + kPi / 2) / omega0;
                const double latency = cons.timeOf(double(analyzerStartFrame + b.startFrame)) - prod.timeOf(srcFrame);
                lastLatency = latency;
                latMin = std::min(latMin, latency);
                latMax = std::max(latMax, latency);
            });
        }
        if (!warm && t >= cfg.warmupSeconds) {
            warm = true;
            analyzer.resetSummary();
        }
        if (warm) {
            fillMin = std::min(fillMin, st.fillError);
            fillMax = std::max(fillMax, st.fillError);
            fifoMin = std::min(fifoMin, st.fifoFrames);
            fifoMax = std::max(fifoMax, st.fifoFrames);
            maxStep = std::max(maxStep, std::abs(st.ratio - prevRatio) * 1e6);
        }
        prevRatio = st.ratio;

        if (progress && cfg.progressEverySec > 0 && t >= nextProgress) {
            nextProgress += cfg.progressEverySec;
            progress({t, st, truePpmAt(t), lastLatency * 1000});
        }

        kC += pC;
        nextC = cons.timeOf(double(kC)) + uni(rng) * cfg.consumer.callbackJitterSec;
    }

    res.stats = bridge.stats();
    res.tone = analyzer.summary();
    res.truePpm = truePpmAt(cfg.seconds);
    res.estimatedPpm = (res.stats.ratio - 1) * 1e6;
    res.latencyDriftMs = latMax >= latMin ? (latMax - latMin) * 1000 : 1e9;
    res.fillErrorMin = fillMin;
    res.fillErrorMax = fillMax;
    res.fifoMin = fifoMin;
    res.fifoMax = fifoMax;
    res.maxRatioStepPpm = maxStep;
    return res;
}

} // namespace dc::clocksync
