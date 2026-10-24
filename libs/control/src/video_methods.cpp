#include "dc/control/video_methods.hpp"

#include "dc/protocol/video.hpp"

namespace dc::control {

using namespace dc::proto;

void registerVideoMethods(ControlServer& server) {
    for (const auto& pattern : videoMethodPatterns()) {
        server.registerMethod(pattern, [](CallContext& ctx) {
            auto o = videoCall(ctx.server.store().tree(), ctx.call.method, ctx.call.args, ctx.client.role);
            if (o.result.code != ErrorCode::Ok) return o.result;
            if (!o.changes.empty() || !o.removed.empty()) {
                ctx.server.publishNow(o.changes, o.removed, ctx.call.id, ctx.client.name);
                ctx.server.publishNow(videoDerived(ctx.server.store().tree()));
            }
            if (!o.event.name.empty()) ctx.server.broadcastEventNow(o.event);
            return o.result;
        });
    }
}

void followVideoDelta(ControlServer& server, const Delta& delta) {
    auto touches = [](const std::string& p) {
        return p.rfind("/video/mix", 0) == 0 || p == "/capture/mix" || p == "/video/vcam/mix" ||
               p == "/video/monitor/mix" || p == "/video/preview/mix";
    };
    bool relevant = false;
    for (const auto& r : delta.removed) relevant = relevant || touches(r);
    for (const auto& c : delta.changes)
        relevant = relevant || (touches(c.path) && c.path.find("/active") == std::string::npos);
    if (relevant) server.publishNow(videoDerived(server.store().tree()));
}

} // namespace dc::control
