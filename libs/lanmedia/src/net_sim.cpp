#include "dc/lanmedia/net_sim.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <queue>
#include <random>
#include <sstream>

namespace dc::lan {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct InFlight {
    double arrival; // true time
    std::uint64_t order;
    std::vector<std::uint8_t> data;
    bool operator>(const InFlight& o) const { return arrival != o.arrival ? arrival > o.arrival : order > o.order; }
};

// Where a run of source frames came from: source frame srcPos was recorded as device frame devFrame.
struct Segment {
    std::uint64_t srcPos;
    std::uint64_t devFrame;
};

} // namespace

std::string SimResult::verdict() const {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << (passed() ? "PASS" : "FAIL") << "  latency " << latencyMinMs << ".." << latencyMaxMs << " ms (mean "
      << latencyMeanMs << "), sent " << datagramsSent << ", lost in network " << datagramsLost << ", concealed "
      << stats.concealed << ", FEC " << stats.fecRecovered << ", late " << stats.late << ", insertions "
      << stats.insertions << ", skips " << stats.skips << ", resyncs " << stats.resyncs << ", glitch blocks "
      << glitchesOutsideEvents << " (all " << tone.glitchBlocks << "/" << tone.blocks << ")"
      << ", drift true " << truePpm << " ppm / estimated " << estimatedPpm << " ppm";
    for (const auto& f : failures) o << "\n    - " << f;
    return o.str();
}

SimResult runSimulation(const SimConfig& cfg, const std::function<void(const SimProgress&)>& progress) {
    const double R = cfg.nominalRate;
    const double sRate = R * (1 + 1e-6 * cfg.sender.ppm);
    const double cRate = R * (1 + 1e-6 * cfg.receiver.ppm);
    auto recvQpc = [&](double t) { return 1000.0 + t * (1 + 1e-6 * cfg.receiverQpcPpm); };

    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::exponential_distribution<double> expo(1.0);

    auto senderCfg = AudioSender::Config{};
    senderCfg.encoder = cfg.encoder;
    senderCfg.nominalRate = R;
    auto sender = std::make_unique<AudioSender>(senderCfg);

    auto rcfg = cfg.receiverConfig;
    rcfg.nominalRate = R;
    AudioReceiver receiver(rcfg);
    const auto outCh = std::size_t(receiver.config().channels);

    auto acfg = cfg.analyzer;
    acfg.sampleRate = R;
    acfg.toneHz = cfg.toneHz;
    clocksync::ToneAnalyzer analyzer(acfg);

    const std::size_t inCh = std::size_t(cfg.encoder.channels);
    const std::size_t sB = cfg.sender.block, cB = cfg.receiver.block;
    std::vector<float> inBuf(sB * inCh), outBuf(cB * outCh);

    std::priority_queue<InFlight, std::vector<InFlight>, std::greater<>> net;
    std::uint64_t order = 0;
    bool burst = false;
    double sendTime = 0;
    SimResult res;
    res.expect = cfg.expect;

    auto inEvent = [&](double t, SimEvent::Kind kind) {
        for (const auto& e : cfg.events)
            if (e.kind == kind && t >= e.atSec && t < e.atSec + e.durationSec) return true;
        return false;
    };
    // glitches and latency are not judged around events
    auto excused = [&](double t) {
        for (const auto& e : cfg.events)
            if (t >= e.atSec && t < e.atSec + e.durationSec + e.settleSec) return true;
        return false;
    };
    std::vector<double> restarts;
    for (const auto& e : cfg.events)
        if (e.kind == SimEvent::Kind::SenderRestart) restarts.push_back(e.atSec);
    std::sort(restarts.begin(), restarts.end());

    AudioSender::Sink sink = [&](std::span<const std::uint8_t> d) {
        ++res.datagramsSent;
        bool lost = inEvent(sendTime, SimEvent::Kind::Outage);
        if (burst) burst = uni(rng) < cfg.network.burstLossStay;
        else burst = uni(rng) < cfg.network.burstLossEnter;
        lost = lost || burst || uni(rng) < cfg.network.lossProbability;
        if (lost) {
            ++res.datagramsLost;
            return;
        }
        const int copies = uni(rng) < cfg.network.duplicateProbability ? 2 : 1;
        for (int i = 0; i < copies; ++i) {
            double delay = cfg.network.baseDelayMs + cfg.network.jitterMeanMs * expo(rng);
            if (uni(rng) < cfg.network.spikeProbability) delay += cfg.network.spikeMs * uni(rng);
            net.push({sendTime + delay / 1000.0, order++, std::vector<std::uint8_t>(d.begin(), d.end())});
        }
    };

    // sender bookkeeping
    std::vector<Segment> segments{{0, 0}};
    std::uint64_t srcPushed = 0;
    bool wasPaused = false;
    std::size_t restartsDone = 0;
    auto captureTime = [&](double srcPos) {
        const Segment* seg = &segments.front();
        for (const auto& s : segments)
            if (double(s.srcPos) <= srcPos) seg = &s;
        return (double(seg->devFrame) + (srcPos - double(seg->srcPos))) / sRate;
    };

    std::uint64_t kS = 0, kC = 0;
    double nextS = double(sB) / sRate + uni(rng) * cfg.sender.callbackJitterSec;
    double nextC = uni(rng) * cfg.receiver.callbackJitterSec;
    double nextReport = 1.0, nextProgress = cfg.progressEverySec > 0 ? cfg.progressEverySec : 1e300;

    bool analyzing = false;
    double analyzeStart = 0;
    double latSum = 0, latLast = 0, ratioSum = 0;
    std::uint64_t latCount = 0;
    res.latencyMinMs = 1e300;
    res.latencyMaxMs = -1e300;

    while (std::min(nextS, nextC) < cfg.seconds) {
        if (nextS <= nextC) {
            // sender device callback for device frames [kS*sB, (kS+1)*sB)
            sendTime = nextS;
            const std::uint64_t dev0 = kS * sB;
            const double t0 = double(dev0) / sRate;
            while (restartsDone < restarts.size() && t0 >= restarts[restartsDone]) {
                sender = std::make_unique<AudioSender>(senderCfg); // new stream id, positions from 0
                srcPushed = 0;
                segments.assign(1, {0, dev0});
                ++restartsDone;
            }
            if (inEvent(t0, SimEvent::Kind::SenderPause)) {
                wasPaused = true;
            } else {
                if (wasPaused) segments.push_back({srcPushed, dev0});
                wasPaused = false;
                for (std::size_t i = 0; i < sB; ++i) {
                    const double ph = 2 * kPi * cfg.toneHz * double(dev0 + i) / R;
                    for (std::size_t c = 0; c < inCh; ++c)
                        inBuf[i * inCh + c] = float(cfg.amplitude * std::sin(ph + double(c) * 0.5));
                }
                sender->push(inBuf.data(), sB, t0);
                srcPushed += sB;
                sender->pump(sink);
            }
            ++kS;
            nextS = double((kS + 1) * sB) / sRate + uni(rng) * cfg.sender.callbackJitterSec;
            continue;
        }

        // receiver device callback for device frames [kC*cB, (kC+1)*cB)
        const double t = nextC;
        while (!net.empty() && net.top().arrival <= t) {
            const auto& p = net.top();
            receiver.onPacket(p.data, recvQpc(p.arrival));
            net.pop();
        }
        const double blockTime = double(kC * cB) / cRate;
        const auto before = receiver.stats();
        receiver.pull(outBuf.data(), cB, recvQpc(blockTime));

        const bool judged = blockTime >= cfg.warmupSeconds && !excused(blockTime);
        if (before.state == AudioReceiver::State::Running && judged) {
            const double lat = (blockTime - captureTime(before.playPosition)) * 1000.0;
            res.latencyMinMs = std::min(res.latencyMinMs, lat);
            res.latencyMaxMs = std::max(res.latencyMaxMs, lat);
            latSum += lat;
            latLast = lat;
            ++latCount;
            ratioSum += before.ratio;
        }
        if (!analyzing && blockTime >= cfg.warmupSeconds) {
            analyzing = true;
            analyzeStart = blockTime;
        }
        if (analyzing) {
            analyzer.feed(outBuf.data(), cB, int(outCh), [&](const clocksync::ToneAnalyzer::Block& b) {
                const double bt = analyzeStart + double(b.startFrame) / cRate;
                if (b.glitch && !excused(bt) && !excused(bt + double(acfg.blockFrames) / cRate))
                    ++res.glitchesOutsideEvents;
            });
        }

        if (t >= nextReport) {
            sender->onReport(receiver.makeReport(recvQpc(t)), t + cfg.network.baseDelayMs / 1000.0);
            nextReport += 1.0;
        }
        if (t >= nextProgress && progress) {
            progress({t, receiver.stats(), latLast});
            nextProgress += cfg.progressEverySec;
        }
        ++kC;
        nextC = double(kC * cB) / cRate + uni(rng) * cfg.receiver.callbackJitterSec;
    }

    res.stats = receiver.stats();
    res.sender = sender->stats();
    res.tone = analyzer.summary();
    res.truePpm = ((1 + 1e-6 * cfg.sender.ppm) / (1 + 1e-6 * cfg.receiver.ppm) - 1) * 1e6;
    if (latCount > 0) {
        res.latencyMeanMs = latSum / double(latCount);
        res.estimatedPpm = (ratioSum / double(latCount) - 1) * 1e6;
    }
    else res.latencyMinMs = res.latencyMaxMs = 0;

    const auto& x = cfg.expect;
    auto fail = [&](std::string s) { res.failures.push_back(std::move(s)); };
    if (latCount == 0) fail("no audio was played");
    if (x.glitchFree && res.glitchesOutsideEvents > 0)
        fail(std::to_string(res.glitchesOutsideEvents) + " glitch blocks outside event windows");
    if (res.latencyMaxMs > x.maxLatencyMs)
        fail("latency " + std::to_string(res.latencyMaxMs) + " ms > " + std::to_string(x.maxLatencyMs) + " ms");
    if (res.latencyMaxMs - res.latencyMinMs > x.maxLatencySpreadMs)
        fail("latency spread " + std::to_string(res.latencyMaxMs - res.latencyMinMs) + " ms > " +
             std::to_string(x.maxLatencySpreadMs) + " ms");
    if (res.stats.resyncs > x.maxResyncs)
        fail(std::to_string(res.stats.resyncs) + " resyncs > " + std::to_string(x.maxResyncs));
    const double concealedPct = res.datagramsSent ? 100.0 * double(res.stats.concealed) / double(res.datagramsSent) : 0;
    if (concealedPct > x.maxConcealedPercent)
        fail("concealed " + std::to_string(concealedPct) + " % > " + std::to_string(x.maxConcealedPercent) + " %");
    if (std::abs(res.estimatedPpm - res.truePpm) > x.maxDriftErrorPpm)
        fail("drift estimate off by " + std::to_string(res.estimatedPpm - res.truePpm) + " ppm");
    return res;
}

std::vector<SimConfig> standardScenarios() {
    std::vector<SimConfig> list;
    auto base = [](const char* name) {
        SimConfig s;
        s.name = name;
        s.sender = {+80, 480, 0.002};   // WASAPI shared mode: 10 ms callbacks, scheduling jitter
        s.receiver = {-40, 480, 0.002};
        s.receiverQpcPpm = 15;
        s.encoder.codec = AudioCodec::Pcm24;
        s.encoder.frames = 120; // 2.5 ms packets
        return s;
    };

    // Wired gigabit LAN, both engines with 10 ms device periods.
    {
        auto s = base("wired-pcm24");
        s.expect.maxLatencyMs = 25;
        list.push_back(s);
    }
    // Wired, low-latency devices (128 frames = 2.7 ms, WASAPI exclusive / ASIO).
    {
        auto s = base("wired-lowlat");
        s.sender = {+80, 128, 0.0005};
        s.receiver = {-40, 128, 0.0005};
        s.expect.maxLatencyMs = 12;
        s.expect.maxLatencySpreadMs = 2;
        list.push_back(s);
    }
    // Cheap crystals far apart.
    {
        auto s = base("wired-large-drift");
        s.encoder.codec = AudioCodec::Pcm16;
        s.sender.ppm = +400;
        s.receiver.ppm = -400;
        s.expect.maxLatencyMs = 25;
        list.push_back(s);
    }
    // Busy switch: heavy reordering (delays spread over 0..8 ms) and duplicates.
    {
        auto s = base("wired-reorder");
        s.network.jitterMeanMs = 2;
        s.network.duplicateProbability = 0.01;
        // exponential delays have no upper bound: a few packets later than anything seen before are
        // concealed
        s.expect.glitchFree = false;
        s.expect.maxConcealedPercent = 0.05;
        s.expect.maxLatencyMs = 50;
        s.expect.maxLatencySpreadMs = 15;
        list.push_back(s);
    }
    // A 500 ms network outage (cable pulled, switch reboot): conceal, then continue at the same latency.
    {
        auto s = base("wired-outage");
        s.events.push_back({SimEvent::Kind::Outage, 120, 0.5});
        s.expect.maxLatencyMs = 25;
        s.expect.maxConcealedPercent = 0.5;
        list.push_back(s);
    }
    // The sender's audio device stops for 1 s (device change) and resumes: one resync.
    {
        auto s = base("sender-pause");
        s.events.push_back({SimEvent::Kind::SenderPause, 120, 1.0});
        s.expect.maxLatencyMs = 25;
        s.expect.maxResyncs = 1;
        s.expect.maxConcealedPercent = 1.0;
        list.push_back(s);
    }
    // The sender restarts the stream (new stream id, positions from 0).
    {
        auto s = base("sender-restart");
        s.events.push_back({SimEvent::Kind::SenderRestart, 120, 0});
        s.expect.maxLatencyMs = 25;
        s.expect.maxConcealedPercent = 0.2;
        list.push_back(s);
    }
    // Wi-Fi with Opus: jitter, occasional 40 ms spikes, 1 % bursty loss. FEC and PLC hide the losses.
    {
        auto s = base("wifi-opus");
        s.encoder.codec = AudioCodec::Opus;
        s.encoder.frames = 480; // 10 ms
        s.network.baseDelayMs = 2;
        s.network.jitterMeanMs = 1.5;
        s.network.spikeProbability = 0.01;
        s.network.spikeMs = 40;
        s.network.lossProbability = 0.005;
        s.network.burstLossEnter = 0.002;
        s.network.burstLossStay = 0.5;
        s.expect.glitchFree = false; // Opus is judged by its counters, not by the tone fit
        s.expect.maxLatencyMs = 80;
        s.expect.maxLatencySpreadMs = 60;
        s.expect.maxConcealedPercent = 1.5;
        // the buffer target follows the spikes, and every move of it shows up in the ratio
        s.expect.maxDriftErrorPpm = 100;
        s.analyzer.glitchSinadDb = 20;
        list.push_back(s);
    }
    // The same Wi-Fi with PCM: the losses become audible concealments; latency must still hold.
    {
        auto s = base("wifi-pcm24");
        s.network = list.back().network;
        s.expect.glitchFree = false;
        s.expect.maxLatencyMs = 80;
        s.expect.maxLatencySpreadMs = 60;
        s.expect.maxConcealedPercent = 3;
        s.expect.maxDriftErrorPpm = 100;
        list.push_back(s);
    }
    return list;
}

} // namespace dc::lan
