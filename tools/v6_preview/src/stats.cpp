#include "stats.hpp"

#include "dc/mfvideo/common.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace v6 {

const char* stageName(int s) {
    static const char* kNames[kStageCount] = {"gen", "encode", "transfer", "decode", "convert", "copy", "present"};
    return (s >= 0 && s < kStageCount) ? kNames[s] : "?";
}

Stats::Stats(Stage finalStage, std::uint64_t warmupFrames) : final_(finalStage), warmup_(warmupFrames), ring_(kRing) {}

void Stats::mark(std::uint64_t id, Stage s) { mark(id, s, nowUs()); }

void Stats::mark(std::uint64_t id, Stage s, std::int64_t tUs) {
    Slot& slot = ring_[id % kRing];
    if (slot.id.load(std::memory_order_acquire) != id) {
        // First stage seen for this id (normally Gen, or Received on a network receiver): claim the slot.
        for (auto& t : slot.t) t.store(-1, std::memory_order_relaxed);
        slot.id.store(id, std::memory_order_release);
    }
    slot.t[s].store(tUs, std::memory_order_release);
    if (s != final_) return;

    FrameTiming ft;
    ft.id = id;
    std::int64_t first = -1, prev = -1;
    for (int k = 0; k <= final_; ++k) {
        const std::int64_t v = slot.t[k].load(std::memory_order_acquire);
        ft.step[k] = (k > 0 && prev >= 0 && v >= 0) ? v - prev : -1;
        if (v >= 0) {
            if (first < 0) first = v;
            prev = v;
        }
    }
    ft.total = first >= 0 ? tUs - first : -1;
    addTiming(ft);
}

void Stats::addTiming(const FrameTiming& t) {
    completed_.fetch_add(1, std::memory_order_relaxed);
    if (t.id < warmup_) return;
    std::lock_guard<std::mutex> lock(mu_);
    timings_.push_back(t);
}

std::int64_t Stats::stamp(std::uint64_t id, Stage s) const {
    const Slot& slot = ring_[id % kRing];
    if (slot.id.load(std::memory_order_acquire) != id) return -1;
    return slot.t[s].load(std::memory_order_acquire);
}

namespace {

Percentiles summarize(std::vector<double> v) {
    Percentiles p;
    p.n = v.size();
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5))]; };
    p.p50 = at(0.50);
    p.p95 = at(0.95);
    p.p99 = at(0.99);
    p.max = v.back();
    double sum = 0;
    for (double x : v) sum += x;
    p.mean = sum / v.size();
    return p;
}

}  // namespace

Percentiles Stats::totalPercentiles(bool recentOnly) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<double> v;
    const size_t from = recentOnly && timings_.size() > 120 ? timings_.size() - 120 : 0;
    for (size_t i = from; i < timings_.size(); ++i)
        if (timings_[i].total >= 0) v.push_back(timings_[i].total / 1000.0);
    return summarize(std::move(v));
}

Percentiles Stats::stepPercentiles(int stage) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<double> v;
    for (const auto& t : timings_)
        if (t.step[stage] >= 0) v.push_back(t.step[stage] / 1000.0);
    return summarize(std::move(v));
}

std::string Stats::liveLine() const {
    const Percentiles p = totalPercentiles(true);
    char buf[200];
    std::snprintf(buf, sizeof buf, "latency p50 %.1f ms  p95 %.1f ms  max %.1f ms  (frames %llu/%llu)", p.p50, p.p95,
                  p.max, static_cast<unsigned long long>(completed()), static_cast<unsigned long long>(generated()));
    return buf;
}

std::string Stats::report(const std::string& title) const {
    std::ostringstream os;
    char buf[200];
    os << "==== " << title << " ====\n";
    os << "stage        n       p50 ms   p95 ms   p99 ms   max ms   mean ms\n";
    for (int s = 1; s <= final_; ++s) {
        const Percentiles p = stepPercentiles(s);
        if (!p.n) continue;
        std::snprintf(buf, sizeof buf, "%-10s %6zu %9.2f %8.2f %8.2f %8.2f %9.2f\n", stageName(s), p.n, p.p50, p.p95,
                      p.p99, p.max, p.mean);
        os << buf;
    }
    const Percentiles t = totalPercentiles(false);
    std::snprintf(buf, sizeof buf, "%-10s %6zu %9.2f %8.2f %8.2f %8.2f %9.2f\n", "TOTAL", t.n, t.p50, t.p95, t.p99,
                  t.max, t.mean);
    os << buf;
    const std::uint64_t gen = generated(), done = completed();
    if (gen) {
        std::snprintf(buf, sizeof buf, "frames: generated %llu, completed %llu (%.1f%% shown)\n",
                      static_cast<unsigned long long>(gen), static_cast<unsigned long long>(done),
                      100.0 * static_cast<double>(done) / static_cast<double>(gen));
        os << buf;
    }
    return os.str();
}

bool Stats::writeCsv(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "frame";
    for (int s = 1; s <= final_; ++s) f << ',' << stageName(s) << "_us";
    f << ",total_us\n";
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& t : timings_) {
        f << t.id;
        for (int s = 1; s <= final_; ++s) f << ',' << t.step[s];
        f << ',' << t.total << '\n';
    }
    return true;
}

}  // namespace v6
