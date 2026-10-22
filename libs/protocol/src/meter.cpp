#include "dc/protocol/meter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dc::proto {

namespace {

template <typename T>
void put(std::vector<std::uint8_t>& out, T v) {
    using U = std::make_unsigned_t<T>;
    auto u = static_cast<U>(v);
    for (std::size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xFF));
}

template <typename T>
T get(const std::uint8_t* p) {
    using U = std::make_unsigned_t<T>;
    U u = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) u = static_cast<U>(u | (static_cast<U>(p[i]) << (8 * i)));
    return static_cast<T>(u);
}

} // namespace

std::vector<std::uint8_t> encodeMeterPacket(const MeterPacket& p) {
    std::vector<std::uint8_t> out;
    const auto count = std::min(p.entries.size(), kMaxMeterEntries);
    out.reserve(kMeterHeaderBytes + count * 12);
    out.insert(out.end(), std::begin(kMeterMagic), std::end(kMeterMagic));
    put<std::uint32_t>(out, p.seq);
    put<std::uint64_t>(out, p.timeUs);
    put<std::uint16_t>(out, static_cast<std::uint16_t>(count));
    put<std::uint16_t>(out, 0); // reserved
    for (std::size_t i = 0; i < count; ++i) {
        const auto& e = p.entries[i];
        const std::uint8_t ch = e.channels == 1 ? 1 : 2;
        put<std::uint16_t>(out, e.meterId);
        put<std::uint8_t>(out, ch);
        put<std::uint8_t>(out, e.flags);
        for (std::uint8_t c = 0; c < ch; ++c) {
            put<std::int16_t>(out, e.peakCdb[c]);
            put<std::int16_t>(out, e.rmsCdb[c]);
        }
    }
    return out;
}

std::optional<MeterPacket> decodeMeterPacket(std::span<const std::uint8_t> data) {
    if (data.size() < kMeterHeaderBytes || std::memcmp(data.data(), kMeterMagic, 4) != 0) return std::nullopt;
    const std::uint8_t* p = data.data();
    MeterPacket out;
    out.seq = get<std::uint32_t>(p + 4);
    out.timeUs = get<std::uint64_t>(p + 8);
    const auto count = get<std::uint16_t>(p + 16);
    if (count > kMaxMeterEntries) return std::nullopt;
    std::size_t pos = kMeterHeaderBytes;
    out.entries.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        if (pos + 4 > data.size()) return std::nullopt;
        MeterEntry e;
        e.meterId = get<std::uint16_t>(p + pos);
        e.channels = p[pos + 2];
        e.flags = p[pos + 3];
        pos += 4;
        if (e.channels != 1 && e.channels != 2) return std::nullopt;
        if (pos + 4u * e.channels > data.size()) return std::nullopt;
        for (std::uint8_t c = 0; c < e.channels; ++c) {
            e.peakCdb[c] = get<std::int16_t>(p + pos);
            e.rmsCdb[c] = get<std::int16_t>(p + pos + 2);
            pos += 4;
        }
        out.entries.push_back(e);
    }
    return out;
}

std::int16_t linearToCdb(float amplitude) {
    if (!(amplitude > 0.0f)) return kMeterSilenceCdb;
    const double db = 20.0 * std::log10(static_cast<double>(amplitude));
    return static_cast<std::int16_t>(std::lround(std::clamp(db, -144.0, 24.0) * 100.0));
}

} // namespace dc::proto
