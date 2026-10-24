#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dc::lan {

// Little-endian stores and loads (independent of the host byte order).
template <typename T> void put(std::uint8_t* out, T v) {
    using U = std::make_unsigned_t<T>;
    auto u = static_cast<U>(v);
    for (std::size_t i = 0; i < sizeof(T); ++i) out[i] = static_cast<std::uint8_t>((u >> (8 * i)) & 0xFF);
}

template <typename T> T get(const std::uint8_t* p) {
    using U = std::make_unsigned_t<T>;
    U u = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) u = static_cast<U>(u | (static_cast<U>(p[i]) << (8 * i)));
    return static_cast<T>(u);
}

} // namespace dc::lan
