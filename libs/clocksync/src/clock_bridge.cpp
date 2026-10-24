#include "dc/clocksync/clock_bridge.hpp"

#include <algorithm>
#include <cstring>

namespace dc::clocksync {

ClockBridge::ClockBridge(Config config)
    : cfg_(config), fifo_(config.channels, config.fifoFrames), dll_(config.nominalRate, config.dllBandwidthHz),
      controller_([&] {
          auto c = config.controller;
          c.nominalRate = config.nominalRate;
          return c;
      }()),
      resampler_(config.channels, config.quality, config.maxBlockFrames) {}

void ClockBridge::push(const float* frames, std::size_t count, double timeOfFirstFrame) {
    const auto pos = fifo_.written();
    const auto stored = fifo_.push(frames, count);
    if (stored < count) {
        overruns_.fetch_add(1, std::memory_order_relaxed);
        producerGap_.store(true, std::memory_order_release);
    }
    if (stored > 0) stamps_.push({pos, timeOfFirstFrame});
}

void ClockBridge::producerDiscontinuity() { producerGap_.store(true, std::memory_order_release); }

void ClockBridge::restart(bool keepRatio) {
    state_ = State::Waiting;
    haveSetpoint_ = false;
    dll_.reset();
    controller_.reset(keepRatio);
    resampler_.reset();
}

void ClockBridge::pull(float* out, std::size_t count, double now) {
    const std::size_t ch = std::size_t(cfg_.channels);
    if (producerGap_.exchange(false, std::memory_order_acq_rel)) {
        if (state_ == State::Running) ++resyncs_;
        restart(true);
        // the stamps already queued describe data before the gap: drop them with the FIFO contents
        Stamp s;
        while (stamps_.pop(s)) {
        }
        fifo_.skip(fifo_.available());
    }
    Stamp s;
    while (stamps_.pop(s)) {
        dll_.update(s.pos, s.time);
        lastStamp_ = s.time;
    }

    if (state_ == State::Waiting) {
        // keep only the newest targetFrames while waiting, so a long wait cannot overflow the FIFO
        if (fifo_.available() > cfg_.targetFrames) fifo_.skip(fifo_.available() - cfg_.targetFrames);
        if (fifo_.available() < cfg_.targetFrames || dll_.updates() < cfg_.startPackets) {
            std::memset(out, 0, count * ch * sizeof(float));
            publish();
            return;
        }
        state_ = State::Running;
        lastPull_ = now;
    }

    // producer frame index the next output frame corresponds to
    double readPos = double(fifo_.read()) - resampler_.bufferedFrames();
    double fill = dll_.positionAt(now) - readPos;
    if (!haveSetpoint_) {
        setpoint_ = fill;
        haveSetpoint_ = true;
    }
    // a backlog the size of the whole target (the consumer stalled, e.g. its device was reopened):
    // drop it now instead of draining it at the ratio limit for a minute
    if (fill - setpoint_ > double(cfg_.targetFrames)) {
        const auto excess = std::min<std::size_t>(std::size_t(fill - setpoint_), fifo_.available());
        fifo_.skip(excess);
        controller_.reset(true);
        ++resyncs_;
        readPos = double(fifo_.read()) - resampler_.bufferedFrames();
        fill = dll_.positionAt(now) - readPos;
    }
    const double dt = std::max(now - lastPull_, 0.0);
    lastPull_ = now;
    const double ratio = controller_.update(fill - setpoint_, dt);

    const std::size_t need = resampler_.inputFramesRequired(count, ratio);
    if (fifo_.available() < need) {
        ++underruns_;
        sUAvail_.store(double(fifo_.available()), std::memory_order_relaxed);
        sUNeed_.store(double(need), std::memory_order_relaxed);
        sUBlock_.store(double(count), std::memory_order_relaxed);
        sUAge_.store(now - lastStamp_, std::memory_order_relaxed);
        std::memset(out, 0, count * ch * sizeof(float));
        restart(true);
        publish();
        return;
    }
    if (need > 0) {
        fifo_.pop(resampler_.inputWritePointer(need), need);
        resampler_.commitInput(need);
    }
    resampler_.produce(out, count, ratio);
    outputFrames_ += count;
    publish();
}

void ClockBridge::publish() {
    sState_.store(int(state_), std::memory_order_relaxed);
    sRatio_.store(controller_.ratio(), std::memory_order_relaxed);
    sFillError_.store(controller_.filteredError(), std::memory_order_relaxed);
    sFifo_.store(double(fifo_.available()), std::memory_order_relaxed);
    sRate_.store(dll_.ready() ? dll_.rate() : 0.0, std::memory_order_relaxed);
    sUnderruns_.store(underruns_, std::memory_order_relaxed);
    sResyncs_.store(resyncs_, std::memory_order_relaxed);
    sOutput_.store(outputFrames_, std::memory_order_relaxed);
    sLocked_.store(state_ == State::Running && controller_.locked(), std::memory_order_relaxed);
}

ClockBridge::Stats ClockBridge::stats() const {
    Stats s;
    s.state = State(sState_.load(std::memory_order_relaxed));
    s.ratio = sRatio_.load(std::memory_order_relaxed);
    s.fillError = sFillError_.load(std::memory_order_relaxed);
    s.fifoFrames = sFifo_.load(std::memory_order_relaxed);
    s.producerRate = sRate_.load(std::memory_order_relaxed);
    s.underruns = sUnderruns_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.resyncs = sResyncs_.load(std::memory_order_relaxed);
    s.outputFrames = sOutput_.load(std::memory_order_relaxed);
    s.locked = sLocked_.load(std::memory_order_relaxed);
    s.lastUnderrunAvailable = sUAvail_.load(std::memory_order_relaxed);
    s.lastUnderrunNeeded = sUNeed_.load(std::memory_order_relaxed);
    s.lastUnderrunBlock = sUBlock_.load(std::memory_order_relaxed);
    s.lastUnderrunStampAge = sUAge_.load(std::memory_order_relaxed);
    return s;
}

} // namespace dc::clocksync
