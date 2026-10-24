#include "dc/lanmedia/video_sim.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <deque>
#include <queue>
#include <random>
#include <sstream>

namespace dc::lan {

namespace {

struct InFlight {
    double arrival;
    std::uint64_t order;
    std::vector<std::uint8_t> data;
    bool operator>(const InFlight& o) const { return arrival != o.arrival ? arrival > o.arrival : order > o.order; }
};

struct ReportInFlight {
    double arrival;
    std::uint64_t order;
    Report report;
    bool operator>(const ReportInFlight& o) const { return arrival != o.arrival ? arrival > o.arrival : order > o.order; }
};

// The bytes of access unit `number` of stream generation `gen`, so the receiver side can check that
// what it got is exactly what was sent.
std::uint8_t pattern(std::uint64_t number, std::uint32_t gen, std::size_t i) {
    return static_cast<std::uint8_t>((number * 131 + gen * 17 + i * 7) & 0xFF);
}

} // namespace

std::string VideoSimResult::verdict() const {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << (passed() ? "PASS" : "FAIL") << "  delivered " << framesDelivered << "/" << framesSent << " ("
      << deliveredPercent << " %), latency mean " << latencyMeanMs << " / p99 " << latencyP99Ms << " / max "
      << latencyMaxMs << " ms, freeze max " << freezeMaxMs << " ms, first frame " << firstFrameMaxMs
      << " ms, datagrams " << datagramsSent << " (lost " << datagramsLost << "), keyframes " << sender.keyframes
      << " (requests " << stats.keyframeRequests << "), dropped frames " << stats.droppedFrames << ", undecodable "
      << undecodable;
    for (const auto& f : failures) o << "\n    - " << f;
    return o.str();
}

VideoSimResult runVideoSimulation(const VideoSimConfig& cfg) {
    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::exponential_distribution<double> expo(1.0);

    VideoSimResult res;
    res.expect = cfg.expect;
    VideoSender sender;
    std::unique_ptr<VideoReceiver> receiver;

    double listenFrom = 0; // the receiver exists from here
    bool joinPending = false;
    for (const auto& e : cfg.events)
        if (e.kind == VideoSimEvent::Kind::Join) {
            listenFrom = e.atSec;
            joinPending = true;
        }
    if (!joinPending) receiver = std::make_unique<VideoReceiver>(cfg.receiver);

    auto inOutage = [&](double t) {
        for (const auto& e : cfg.events)
            if (e.kind == VideoSimEvent::Kind::Outage && t >= e.atSec && t < e.atSec + e.durationSec) return true;
        return false;
    };
    // freezes are not judged around events
    auto excused = [&](double t) {
        for (const auto& e : cfg.events)
            if (t >= e.atSec - 0.2 && t < e.atSec + e.durationSec + 1.0) return true;
        return false;
    };

    std::priority_queue<InFlight, std::vector<InFlight>, std::greater<>> forward;
    std::priority_queue<ReportInFlight, std::vector<ReportInFlight>, std::greater<>> backward;
    std::uint64_t order = 0;
    double linkFree = 0;
    bool burst = false;
    double now = 0;

    VideoSender::Sink sink = [&](std::span<const std::uint8_t> d) {
        ++res.datagramsSent;
        const double depart = std::max(now, linkFree);
        linkFree = depart + double(d.size()) * 8.0 / (cfg.linkMbps * 1e6);
        if (burst) burst = uni(rng) < cfg.network.burstLossStay;
        else burst = uni(rng) < cfg.network.burstLossEnter;
        if (inOutage(now) || burst || uni(rng) < cfg.network.lossProbability) {
            ++res.datagramsLost;
            return;
        }
        const int copies = uni(rng) < cfg.network.duplicateProbability ? 2 : 1;
        for (int i = 0; i < copies; ++i) {
            double delay = cfg.network.baseDelayMs + cfg.network.jitterMeanMs * expo(rng);
            if (uni(rng) < cfg.network.spikeProbability) delay += cfg.network.spikeMs * uni(rng);
            forward.push({linkFree + delay / 1000.0, order++, std::vector<std::uint8_t>(d.begin(), d.end())});
        }
    };

    // receiver side bookkeeping
    std::uint32_t generation = 0;                 // sender restarts
    std::vector<std::uint32_t> streamGeneration;  // stream id -> generation, by insertion
    std::vector<std::uint32_t> streamIds;
    std::vector<double> latencies;
    bool haveLast = false, lastOk = false;
    std::uint32_t lastStream = 0;
    std::uint64_t lastNumber = 0;
    double lastCapture = -1;
    double waitFirstSince = joinPending ? listenFrom : 0; // >= 0: waiting for the first picture
    std::uint64_t framesWhileListening = 0;

    auto genOf = [&](std::uint32_t id) -> std::uint32_t {
        for (std::size_t i = 0; i < streamIds.size(); ++i)
            if (streamIds[i] == id) return streamGeneration[i];
        return 0;
    };

    VideoReceiver::FrameHandler onFrame = [&](const VideoReceiver::Frame& f) {
        ++res.framesDelivered;
        bool ok = f.keyframe || (haveLast && lastOk && f.streamId == lastStream && f.number == lastNumber + 1);
        // the content must be the access unit that was sent
        const std::uint32_t gen = genOf(f.streamId);
        for (std::size_t i = 0; i < f.data.size() && ok; i += 97)
            if (f.data[i] != pattern(f.number, gen, i)) ok = false;
        if (!ok) ++res.undecodable;
        haveLast = true;
        lastOk = ok;
        lastStream = f.streamId;
        lastNumber = f.number;
        latencies.push_back((f.arrival - f.captureTime) * 1000.0);
        if (waitFirstSince >= 0) {
            res.firstFrameMaxMs = std::max(res.firstFrameMaxMs, (now - waitFirstSince) * 1000.0);
            waitFirstSince = -1;
        } else if (lastCapture >= 0 && !excused(f.captureTime)) {
            res.freezeMaxMs = std::max(res.freezeMaxMs, (f.captureTime - lastCapture) * 1000.0);
        }
        lastCapture = f.captureTime;
    };

    const double dt = 0.0001;
    const double framePeriod = 1.0 / cfg.encoder.fps;
    std::uint64_t frameIndex = 0;   // captures so far
    std::uint64_t sinceKey = 0;
    bool forceKey = true;
    double nextPoll = 0;
    struct Pending {
        double capture;
        double ready;
    };
    std::deque<Pending> encoding;
    std::vector<VideoSimEvent> restarts;
    for (const auto& e : cfg.events)
        if (e.kind == VideoSimEvent::Kind::SenderRestart) restarts.push_back(e);
    std::size_t restartsDone = 0;
    std::vector<std::uint8_t> au;
    std::uint64_t streamFrame = 0; // mirrors the sender's frame numbering (from 0 per stream)

    streamIds.push_back(sender.streamId());
    streamGeneration.push_back(generation);

    const auto steps = static_cast<std::uint64_t>(cfg.seconds / dt);
    for (std::uint64_t step = 0; step <= steps; ++step) {
        now = double(step) * dt;

        if (joinPending && now >= listenFrom) {
            receiver = std::make_unique<VideoReceiver>(cfg.receiver);
            joinPending = false;
        }
        if (restartsDone < restarts.size() && now >= restarts[restartsDone].atSec) {
            ++restartsDone;
            sender.restart();
            streamFrame = 0;
            ++generation;
            streamIds.push_back(sender.streamId());
            streamGeneration.push_back(generation);
            forceKey = true;
            if (waitFirstSince < 0) waitFirstSince = now;
            haveLast = false;
            lastCapture = -1;
        }

        // capture and encode
        while (double(frameIndex) * framePeriod <= now) {
            encoding.push_back({double(frameIndex) * framePeriod, double(frameIndex) * framePeriod + cfg.encoder.encodeMs / 1000.0});
            ++frameIndex;
        }
        while (!encoding.empty() && encoding.front().ready <= now) {
            const auto p = encoding.front();
            encoding.pop_front();
            const bool key = forceKey || sender.keyframeWanted() ||
                             (cfg.encoder.gopFrames > 0 && sinceKey + 1 >= std::uint64_t(cfg.encoder.gopFrames));
            forceKey = false;
            sinceKey = key ? 0 : sinceKey + 1;
            const double base = double(key ? cfg.encoder.keyframeBytes : cfg.encoder.frameBytes);
            const auto bytes = static_cast<std::size_t>(std::max(16.0, base * (key ? 1.0 : 0.7 + 0.6 * uni(rng))));
            au.resize(bytes);
            for (std::size_t k = 0; k < bytes; ++k) au[k] = pattern(streamFrame, generation, k);
            ++streamFrame;
            ++res.framesSent;
            if (receiver) ++framesWhileListening;
            sender.send(au, key, p.capture, now, sink);
        }

        // network: forward
        while (!forward.empty() && forward.top().arrival <= now) {
            auto pkt = forward.top();
            forward.pop();
            if (receiver) receiver->onPacket(pkt.data, now, onFrame);
        }
        // network: reports back
        while (!backward.empty() && backward.top().arrival <= now) {
            sender.onReport(backward.top().report, now);
            backward.pop();
        }
        if (receiver && now >= nextPoll) {
            nextPoll = now + cfg.pollMs / 1000.0;
            if (auto r = receiver->poll(now, onFrame)) {
                if (!inOutage(now) && uni(rng) >= cfg.network.lossProbability) {
                    const double delay = cfg.network.baseDelayMs + cfg.network.jitterMeanMs * expo(rng);
                    backward.push({now + delay / 1000.0, order++, *r});
                }
            }
        }
    }

    if (receiver) res.stats = receiver->stats();
    res.sender = sender.stats();
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        double sum = 0;
        for (double l : latencies) sum += l;
        res.latencyMeanMs = sum / double(latencies.size());
        res.latencyP99Ms = latencies[std::min(latencies.size() - 1, std::size_t(double(latencies.size()) * 0.99))];
        res.latencyMaxMs = latencies.back();
    }
    res.deliveredPercent = framesWhileListening ? 100.0 * double(res.framesDelivered) / double(framesWhileListening) : 0;
    if (waitFirstSince >= 0) res.firstFrameMaxMs = 1e9; // never got a picture

    const auto& x = cfg.expect;
    auto fail = [&](std::string s) { res.failures.push_back(std::move(s)); };
    if (res.undecodable) fail("undecodable frames delivered: " + std::to_string(res.undecodable));
    if (res.deliveredPercent < x.minDeliveredPercent) fail("delivered " + std::to_string(res.deliveredPercent) + " %");
    if (res.latencyP99Ms > x.maxLatencyMs) fail("latency p99 " + std::to_string(res.latencyP99Ms) + " ms");
    if (res.freezeMaxMs > x.maxFreezeMs) fail("freeze " + std::to_string(res.freezeMaxMs) + " ms");
    if (res.firstFrameMaxMs > x.maxFirstFrameMs) fail("first frame after " + std::to_string(res.firstFrameMaxMs) + " ms");
    return res;
}

std::vector<VideoSimConfig> standardVideoScenarios() {
    std::vector<VideoSimConfig> v;
    auto base = [] {
        VideoSimConfig c;
        c.encoder.fps = 30; // remote preview: 960x540, 3 Mbps
        c.encoder.keyframeBytes = 40000;
        c.encoder.frameBytes = 12000;
        c.encoder.gopFrames = 0; // keyframes only when asked (and at the start)
        c.network.baseDelayMs = 0.3;
        c.network.jitterMeanMs = 0.05;
        c.linkMbps = 1000;
        return c;
    };
    {
        auto c = base();
        c.name = "preview-wired";
        c.expect = {100.0, 5, 40, 20};
        v.push_back(c);
    }
    {
        auto c = base();
        c.name = "preview-wired-loss";
        c.network.lossProbability = 0.001;
        c.network.duplicateProbability = 0.01;
        c.expect = {97.0, 5, 200, 20}; // a lost keyframe costs one retry (0.1 s)
        v.push_back(c);
    }
    {
        auto c = base();
        c.name = "preview-wifi";
        c.linkMbps = 60;
        c.network.baseDelayMs = 2;
        c.network.jitterMeanMs = 1;
        c.network.spikeProbability = 0.01;
        c.network.spikeMs = 40;
        c.network.lossProbability = 0.002;
        c.network.burstLossEnter = 0.0005;
        c.network.burstLossStay = 0.5;
        c.expect = {93.0, 60, 300, 120};
        v.push_back(c);
    }
    {
        auto c = base();
        c.name = "preview-outage";
        c.events = {{VideoSimEvent::Kind::Outage, 20, 0.5}};
        c.expect = {97.0, 5, 40, 20};
        v.push_back(c);
    }
    {
        auto c = base();
        c.name = "preview-restart";
        c.events = {{VideoSimEvent::Kind::SenderRestart, 15, 0}, {VideoSimEvent::Kind::SenderRestart, 45, 0}};
        c.expect = {99.0, 5, 40, 80};
        v.push_back(c);
    }
    {
        auto c = base();
        c.name = "preview-join";
        c.encoder.gopFrames = 600; // periodic keyframes far apart: the joining viewer must ask
        c.events = {{VideoSimEvent::Kind::Join, 7.3, 0}};
        c.expect = {99.0, 5, 40, 80};
        v.push_back(c);
    }
    {
        auto c = base();
        c.name = "lan-1080p-wired";
        c.encoder.keyframeBytes = 200000; // 1080p30, 8 Mbps
        c.encoder.frameBytes = 32000;
        c.encoder.gopFrames = 60;
        c.linkMbps = 1000;
        c.expect = {100.0, 8, 40, 20};
        v.push_back(c);
    }
    return v;
}

} // namespace dc::lan
