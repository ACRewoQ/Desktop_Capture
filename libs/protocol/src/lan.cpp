#include "dc/protocol/lan.hpp"

#include "dc/protocol/schema.hpp"

#include <charconv>

namespace dc::proto {

std::vector<LanSubscription> lanSubscriptions(const ParamTree& tree, const std::string& stream) {
    std::vector<LanSubscription> out;
    for (int id = 1; id <= kMaxLanSubscriptions; ++id) {
        const auto base = "/lan/sub/" + std::to_string(id);
        const auto s = tree.getAs<std::string>(base + "/stream");
        const auto a = tree.getAs<std::string>(base + "/address");
        if (!s || !a || (!stream.empty() && *s != stream)) continue;
        const auto colon = a->rfind(':');
        if (colon == std::string::npos || colon == 0) continue;
        int port = 0;
        const char* first = a->data() + colon + 1;
        const char* last = a->data() + a->size();
        auto [p, ec] = std::from_chars(first, last, port);
        if (ec != std::errc() || p != last || port < 1 || port > 65535) continue;
        out.push_back({id, *s, a->substr(0, colon), static_cast<std::uint16_t>(port),
                       tree.getAs<std::string>(base + "/client").value_or("")});
    }
    return out;
}

} // namespace dc::proto
