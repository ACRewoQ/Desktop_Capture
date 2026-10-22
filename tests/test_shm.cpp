#include "dc/shmaudio/clock.hpp"
#include "dc/shmaudio/ring.hpp"
#include "dc/shmaudio/shared_memory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <memory>
#include <thread>
#include <vector>

using namespace dc::shm;

namespace {

struct Memory {
    // 64-byte aligned, zeroed heap block standing in for the mapping
    Memory() : ptr(static_cast<std::uint8_t*>(::operator new(kLayout.totalBytes, std::align_val_t{4096}))) {
        std::memset(ptr, 0, kLayout.totalBytes);
    }
    ~Memory() { ::operator delete(ptr, std::align_val_t{4096}); }
    std::uint8_t* ptr;
};

// Writes a ramp so every sample identifies its frame: value = frame (+0.5 on the right channel).
void writeRamp(AudioRingWriter& w, std::uint32_t slot, std::uint32_t frames, std::uint32_t channels) {
    std::vector<float> buf(frames * channels);
    const auto start = w.framesWritten();
    for (std::uint32_t i = 0; i < frames; ++i)
        for (std::uint32_t c = 0; c < channels; ++c) buf[i * channels + c] = float(start + i) + (c ? 0.5f : 0.0f);
    w.write(slot, buf.data(), frames, channels);
}

} // namespace

TEST_CASE("layout is page aligned and stable", "[shm]") {
    CHECK(kLayout.slotsOffset % kPageBytes == 0);
    CHECK(kLayout.dataOffset % kPageBytes == 0);
    CHECK(kLayout.slotDataBytes == kCapacityFrames * 2 * 4);
    CHECK(kLayout.totalBytes == kLayout.dataOffset + kLayout.slotDataBytes * kSlotCount);
    CHECK(sizeof(SharedHeader) <= kLayout.slotsOffset);
    // values quoted in docs/spec/shm-audio.md
    CHECK(kLayout.slotsOffset == 4096);
    CHECK(kLayout.dataOffset == 12288);
    CHECK(kLayout.totalBytes == 34091008);
}

TEST_CASE("reader receives what the writer commits, aligned across slots", "[shm]") {
    Memory m;
    AudioRingWriter w(m.ptr, 1);
    std::string err;
    auto r = AudioRingReader::attach(m.ptr, kLayout.totalBytes, &err);
    REQUIRE(r.has_value());

    w.configureSlot(0, 2, "master");
    w.configureSlot(1, 1, "track/1");
    for (int b = 0; b < 4; ++b) {
        writeRamp(w, 0, 256, 2);
        writeRamp(w, 1, 256, 1);
        w.commit(256, nowQpc());
    }
    CHECK(r->available() == 1024);

    std::vector<float> a(2048), b(2048), c(2048);
    auto i0 = r->read(0, a, 1024, false);
    auto i1 = r->read(1, b, 1024, false);
    auto i2 = r->read(2, c, 1024, false); // unused slot
    CHECK(i2.frames == 1024);
    r->advance(1024);
    CHECK(i0.status == AudioRingReader::Status::Ok);
    CHECK(i0.frames == 1024);
    CHECK(i1.frames == 1024);
    CHECK(i0.startFrame == i1.startFrame);
    for (std::uint32_t i = 0; i < 1024; ++i) {
        REQUIRE(a[i * 2] == float(i));
        REQUIRE(a[i * 2 + 1] == float(i) + 0.5f);
        REQUIRE(b[i * 2] == float(i));
        REQUIRE(b[i * 2 + 1] == 0.0f); // mono: right channel is zero
        REQUIRE(c[i * 2] == 0.0f);
    }
    CHECK(r->read(0, a, 1024).status == AudioRingReader::Status::NoData);
}

TEST_CASE("a slot activated later is silent before its activation frame", "[shm]") {
    Memory m;
    AudioRingWriter w(m.ptr, 1);
    auto r = AudioRingReader::attach(m.ptr, kLayout.totalBytes, nullptr);
    REQUIRE(r);
    w.commit(128, nowQpc()); // slot 3 not yet active
    w.configureSlot(3, 2, "track/3");
    writeRamp(w, 3, 128, 2);
    w.commit(128, nowQpc());
    std::vector<float> out(512);
    auto info = r->read(3, out, 256);
    REQUIRE(info.frames == 256);
    CHECK(out[127 * 2] == 0.0f);
    CHECK(out[128 * 2] == 128.0f);
}

TEST_CASE("a slow reader detects overruns and recovers", "[shm]") {
    Memory m;
    AudioRingWriter w(m.ptr, 1);
    auto r = AudioRingReader::attach(m.ptr, kLayout.totalBytes, nullptr);
    REQUIRE(r);
    w.configureSlot(0, 2, "master");
    const std::uint32_t blocks = kCapacityFrames / 1024 + 4;
    for (std::uint32_t b = 0; b < blocks; ++b) {
        writeRamp(w, 0, 1024, 2);
        w.commit(1024, nowQpc());
    }
    std::vector<float> out(8192);
    auto info = r->read(0, out, 4096);
    CHECK(info.status == AudioRingReader::Status::Overrun);
    CHECK(info.skippedFrames > 0);
    REQUIRE(info.frames == 4096);
    CHECK(out[0] == float(info.startFrame)); // data after the jump is correct
    CHECK(r->read(0, out, 4096).status == AudioRingReader::Status::Ok);
}

TEST_CASE("a restarted writer is detected through the generation", "[shm]") {
    Memory m;
    auto w1 = std::make_unique<AudioRingWriter>(m.ptr, 1);
    auto r = AudioRingReader::attach(m.ptr, kLayout.totalBytes, nullptr);
    REQUIRE(r);
    w1->commit(512, nowQpc());
    std::vector<float> out(2048);
    CHECK(r->read(0, out, 512).frames == 512);

    AudioRingWriter w2(m.ptr, 2); // engine restarted on the same mapping
    w2.commit(256, nowQpc());
    auto info = r->read(0, out, 512);
    CHECK(info.status == AudioRingReader::Status::WriterRestarted);
    CHECK(r->position() == 256);
    w2.commit(256, nowQpc());
    CHECK(r->read(0, out, 512).frames == 256);
}

TEST_CASE("frames map to QPC through time anchors", "[shm]") {
    Memory m;
    AudioRingWriter w(m.ptr, 1);
    auto r = AudioRingReader::attach(m.ptr, kLayout.totalBytes, nullptr);
    REQUIRE(r);
    CHECK_FALSE(r->qpcForFrame(0).has_value());
    const std::int64_t f = qpcFrequency();
    const std::int64_t t0 = 1'000'000;
    for (int b = 0; b < 100; ++b) w.commit(480, t0 + b * f / 100); // 10 ms blocks
    CHECK(r->qpcForFrame(0) == t0);
    CHECK(r->qpcForFrame(480 * 50) == t0 + 50 * f / 100);
    const auto mid = r->qpcForFrame(480 * 50 + 240); // half a block later
    REQUIRE(mid.has_value());
    CHECK(std::llabs(*mid - (t0 + 50 * f / 100 + f / 200)) <= 1);
    CHECK(r->writerAlive(t0 + 99 * f / 100 + f / 10, f / 5));
    CHECK_FALSE(r->writerAlive(t0 + 99 * f / 100 + f, f / 5));
}

TEST_CASE("attach rejects foreign memory", "[shm]") {
    Memory m;
    std::string err;
    CHECK_FALSE(AudioRingReader::attach(m.ptr, kLayout.totalBytes, &err).has_value()); // all zero
    CHECK(err == "bad magic");
    AudioRingWriter w(m.ptr, 1);
    CHECK_FALSE(AudioRingReader::attach(m.ptr, 1024, &err).has_value());
}

TEST_CASE("concurrent writer and reader stay consistent", "[shm]") {
    Memory m;
    AudioRingWriter w(m.ptr, 1);
    w.configureSlot(0, 2, "master");
    auto r = AudioRingReader::attach(m.ptr, kLayout.totalBytes, nullptr);
    REQUIRE(r);
    constexpr std::uint64_t kTotal = 48000 * 4;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        while (w.framesWritten() < kTotal) {
            writeRamp(w, 0, 256, 2);
            w.commit(256, nowQpc());
            if (w.framesWritten() % 4096 == 0) std::this_thread::yield();
        }
        done = true;
    });
    std::vector<float> out(8192);
    std::uint64_t checked = 0, bad = 0;
    while (!done || r->available() > 0) {
        auto info = r->read(0, out, 4096);
        for (std::uint32_t i = 0; i < info.frames; ++i) {
            if (out[i * 2] != float(info.startFrame + i)) ++bad;
            ++checked;
        }
    }
    writer.join();
    CHECK(bad == 0);
    CHECK(checked > 0);
}

TEST_CASE("named shared memory can be created and opened", "[shm]") {
    const std::string suffix = ".test." + std::to_string(nowQpc());
    std::string err;
    auto owner = SharedMemory::create(kLayout.totalBytes, suffix, &err);
    REQUIRE(owner.valid());
    AudioRingWriter w(owner.data(), 1);
    w.commit(64, nowQpc());
    auto view = SharedMemory::open(kLayout.totalBytes, suffix, &err);
    REQUIRE(view.valid());
    auto r = AudioRingReader::attach(view.data(), view.size(), &err);
    REQUIRE(r.has_value());
    CHECK(r->header()->framesWritten.load() == 64);
}
