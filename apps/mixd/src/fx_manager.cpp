#include "fx_manager.hpp"

#include "fx/builtin.hpp"

#include "dc/protocol/fx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>

namespace dc::mixd {

using namespace dc::proto;
using control::CallContext;

namespace {

constexpr int kInitialPublishedParams = 128;
constexpr int kDefaultParamsCount = 64;
constexpr int kMaxParamsCount = 256;

Result error(ErrorCode code, std::string message) {
    Result r;
    r.code = code;
    r.message = std::move(message);
    return r;
}

std::optional<std::int64_t> intArg(const Args& args, const char* key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::int64_t>(&it->second)) return *v;
    return std::nullopt;
}
std::optional<std::string> strArg(const Args& args, const char* key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::string>(&it->second)) return *v;
    return std::nullopt;
}
bool boolArg(const Args& args, const char* key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<bool>(&it->second)) return *v;
    return false;
}

// Where a path points inside an fx rack: strip (0 = master), slot (1-based), the rest ("param/ratio").
struct FxPath {
    int strip = -1;
    int slot = 0;
    std::string rest;
};
std::optional<FxPath> parseFxPath(const std::string& path) {
    FxPath p;
    std::size_t pos = 0;
    if (path.rfind("/master/fx/", 0) == 0) {
        p.strip = 0;
        pos = 11;
    } else if (path.rfind("/track/", 0) == 0) {
        std::size_t end = path.find('/', 7);
        if (end == std::string::npos || path.compare(end, 4, "/fx/") != 0) return std::nullopt;
        p.strip = std::atoi(path.substr(7, end - 7).c_str());
        pos = end + 4;
    } else {
        return std::nullopt;
    }
    const std::size_t end = path.find('/', pos);
    p.slot = std::atoi(path.substr(pos, end == std::string::npos ? std::string::npos : end - pos).c_str());
    if (end != std::string::npos) p.rest = path.substr(end + 1);
    if (p.strip < 0 || p.strip >= kMaxStrips || p.slot < 1) return std::nullopt;
    return p;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

std::string jsonNumber(double v) {
    if (!std::isfinite(v)) return "0";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

// The display text of a built-in parameter value, e.g. "-6.0 dB", "2.50 kHz", "25%".
std::string builtinText(const FxParamSpec& p, double v) {
    char buf[48];
    if (p.steps == 2) return v >= 0.5 ? "オン" : "オフ";
    if (p.unit == "dB") std::snprintf(buf, sizeof buf, "%.1f dB", v);
    else if (p.unit == "Hz") {
        if (v >= 1000) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000);
        else std::snprintf(buf, sizeof buf, "%.0f Hz", v);
    } else if (p.unit == "ms") std::snprintf(buf, sizeof buf, v < 10 ? "%.2f ms" : "%.0f ms", v);
    else if (p.unit == ":1") std::snprintf(buf, sizeof buf, "%.1f:1", v);
    else if (p.max <= 1.0 && p.min >= 0.0) std::snprintf(buf, sizeof buf, "%.0f%%", v * 100);
    else std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

std::string paramJson(const std::string& key, const std::string& name, const std::string& unit, double min, double max,
                      double def, double value, const std::string& text, int steps, bool log) {
    return "{\"key\":" + jsonString(key) + ",\"name\":" + jsonString(name) + ",\"unit\":" + jsonString(unit) +
           ",\"min\":" + jsonNumber(min) + ",\"max\":" + jsonNumber(max) + ",\"default\":" + jsonNumber(def) +
           ",\"value\":" + jsonNumber(value) + ",\"text\":" + jsonString(text) + ",\"steps\":" + std::to_string(steps) +
           ",\"log\":" + (log ? "true" : "false") + "}";
}

std::vector<std::string> splitKeys(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty()) out.push_back(item);
    return out;
}

std::vector<std::string> splitDirs(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ';'))
        if (!item.empty()) out.push_back(item);
    return out;
}

} // namespace

std::string stripBase(int strip) { return strip == 0 ? std::string("/master") : "/track/" + std::to_string(strip); }

bool FxManager::handlesPath(const std::string& path) {
    return path.rfind("/plugins/", 0) == 0 || parseFxPath(path).has_value() || path == "/master/fx" ||
           (path.size() > 3 && path.compare(path.size() - 3, 3, "/fx") == 0);
}

FxManager::FxManager(asio::io_context& io, control::ControlServer& server, PluginHost* host,
                     std::function<void()> onRackChanged, bool verbose)
    : io_(io), server_(server), host_(host), onRackChanged_(std::move(onRackChanged)), verbose_(verbose), flushTimer_(io) {}

FxManager::~FxManager() { alive_.reset(); }

void FxManager::start() {
    auto reg = [this](const std::string& pattern, proto::Result (FxManager::*fn)(CallContext&)) {
        server_.registerMethod(pattern, [this, fn](CallContext& ctx) { return (this->*fn)(ctx); });
    };
    for (const std::string base : {"/track/#", "/master"}) {
        reg(base + "/fx/add", &FxManager::add);
        reg(base + "/fx/remove", &FxManager::remove);
        reg(base + "/fx/move", &FxManager::move);
        reg(base + "/fx/#/params", &FxManager::params);
    }
    reg("/plugins/scan", &FxManager::scan);
    publishPlugins();
}

void FxManager::stop() {
    flushTimer_.cancel();
    alive_.reset();
    for (int s = 0; s < kMaxStrips; ++s) clearStrip(s);
}

int FxManager::stripOfMethod(const std::string& method) const {
    if (method.rfind("/master/", 0) == 0) return 0;
    if (method.rfind("/track/", 0) != 0) return -1;
    const int n = std::atoi(method.c_str() + 7);
    if (n < 1 || n >= kMaxStrips || !server_.store().tree().contains(stripBase(n) + "/position")) return -1;
    return n;
}

std::vector<FxManager::SlotValues> FxManager::readSlots(int strip) const {
    const auto& tree = server_.store().tree();
    std::vector<SlotValues> out;
    for (int m = 1; m <= static_cast<int>(strips_[static_cast<std::size_t>(strip)].size()); ++m) {
        const auto prefix = stripBase(strip) + "/fx/" + std::to_string(m);
        SlotValues v;
        for (const auto& path : tree.pathsUnder(prefix)) v.push_back({path.substr(prefix.size()), *tree.get(path)});
        out.push_back(std::move(v));
    }
    return out;
}

void FxManager::writeSlots(int strip, const std::vector<SlotValues>& slots, std::uint64_t originId, const std::string& origin) {
    std::vector<Change> c;
    for (std::size_t m = 0; m < slots.size(); ++m) {
        const auto prefix = stripBase(strip) + "/fx/" + std::to_string(m + 1);
        for (const auto& [suffix, value] : slots[m]) c.push_back({prefix + suffix, value});
    }
    server_.publishNow(c, {stripBase(strip) + "/fx"}, originId, origin);
}

FxManager::Entry* FxManager::findSerial(std::uint64_t serial, int* strip, int* index) {
    for (int s = 0; s < kMaxStrips; ++s) {
        auto& v = strips_[static_cast<std::size_t>(s)];
        for (std::size_t i = 0; i < v.size(); ++i)
            if (v[i].serial == serial) {
                if (strip) *strip = s;
                if (index) *index = static_cast<int>(i);
                return &v[i];
            }
    }
    return nullptr;
}

void FxManager::updateRack(int strip) {
    auto& r = rack_[static_cast<std::size_t>(strip)];
    r.clear();
    for (const auto& e : strips_[static_cast<std::size_t>(strip)])
        if (e.slot) r.push_back(e.slot);
    if (onRackChanged_) onRackChanged_();
}

void FxManager::clearStrip(int strip) {
    auto& v = strips_[static_cast<std::size_t>(strip)];
    if (v.empty()) return;
    for (auto& e : v)
        if (e.plugin) e.plugin->setListener({});
    v.clear();
    updateRack(strip);
}

// --- methods ---

Result FxManager::add(CallContext& ctx) {
    const int strip = stripOfMethod(ctx.call.method);
    if (strip < 0) return error(ErrorCode::NotFound, "no such track");
    auto& entries = strips_[static_cast<std::size_t>(strip)];
    const auto type = strArg(ctx.call.args, "type");
    if (!type) return error(ErrorCode::BadRequest, "type is required");
    const int count = static_cast<int>(entries.size());
    if (count >= kMaxFxPerTrack) return error(ErrorCode::LimitReached, "16 effects already");
    const auto index = intArg(ctx.call.args, "index").value_or(count + 1);
    if (index < 1 || index > count + 1) return error(ErrorCode::OutOfRange, "index out of range");

    Entry e;
    e.serial = nextSerial_++;
    e.type = *type;
    SlotValues values;
    if (const auto* spec = findBuiltinFx(*type)) {
        auto fx = makeBuiltinEffect(*type);
        e.effect = fx;
        e.slot = std::make_shared<FxSlot>(fx);
        values = {{"/type", *type},
                  {"/name", spec->name},
                  {"/bypass", false},
                  {"/status", std::string("ok")},
                  {"/error", std::string()},
                  {"/latency_samples", std::int64_t{spec->latencySamples}},
                  {"/param_count", static_cast<std::int64_t>(spec->params.size())},
                  {"/editor", false}};
        for (const auto& p : spec->params) values.push_back({"/param/" + p.key, p.defaultValue});
    } else if (isVst3Type(*type)) {
        if (!host_) return error(ErrorCode::NotFound, "VST3 plugins are not available in this build of the engine");
        const auto list = host_->plugins();
        const auto it = std::find_if(list.begin(), list.end(), [&](const PluginInfo& p) { return p.id == *type; });
        if (it == list.end()) return error(ErrorCode::NotFound, "unknown plugin " + *type + " (scan with /plugins/scan)");
        if (it->status != "ok") return error(ErrorCode::Conflict, it->name + " did not pass the scan (" + it->status + ")");
        values = {{"/type", *type},
                  {"/name", it->name},
                  {"/bypass", false},
                  {"/status", std::string("loading")},
                  {"/error", std::string()},
                  {"/latency_samples", std::int64_t{0}},
                  {"/param_count", std::int64_t{0}},
                  {"/editor", false}};
        const auto serial = e.serial;
        std::weak_ptr<int> alive = alive_;
        host_->create(*type, [this, alive, serial](std::shared_ptr<PluginInstance> plugin, std::string err) {
            if (alive.expired()) return;
            asio::post(io_, [this, alive, serial, plugin = std::move(plugin), err = std::move(err)]() mutable {
                if (!alive.expired()) onPluginLoaded(serial, std::move(plugin), err);
            });
        });
    } else {
        return error(ErrorCode::NotFound, "unknown effect type " + *type);
    }

    auto slots = readSlots(strip);
    slots.insert(slots.begin() + (index - 1), std::move(values));
    entries.insert(entries.begin() + (index - 1), std::move(e));
    writeSlots(strip, slots, ctx.call.id, ctx.client.name);
    updateRack(strip);
    if (verbose_) std::cout << "fx add " << stripBase(strip) << "/fx/" << index << " " << *type << std::endl;
    Result r;
    r.value = std::int64_t{index};
    return r;
}

Result FxManager::remove(CallContext& ctx) {
    const int strip = stripOfMethod(ctx.call.method);
    if (strip < 0) return error(ErrorCode::NotFound, "no such track");
    auto& entries = strips_[static_cast<std::size_t>(strip)];
    const auto index = intArg(ctx.call.args, "index");
    if (!index || *index < 1 || *index > static_cast<std::int64_t>(entries.size()))
        return error(ErrorCode::NotFound, "no such effect slot");
    auto slots = readSlots(strip);
    slots.erase(slots.begin() + (*index - 1));
    auto& e = entries[static_cast<std::size_t>(*index - 1)];
    if (e.plugin) {
        e.plugin->setListener({});
        e.plugin->setEditorOpen(false);
    }
    entries.erase(entries.begin() + (*index - 1));
    writeSlots(strip, slots, ctx.call.id, ctx.client.name);
    updateRack(strip);
    return {};
}

Result FxManager::move(CallContext& ctx) {
    const int strip = stripOfMethod(ctx.call.method);
    if (strip < 0) return error(ErrorCode::NotFound, "no such track");
    auto& entries = strips_[static_cast<std::size_t>(strip)];
    const auto n = static_cast<std::int64_t>(entries.size());
    const auto from = intArg(ctx.call.args, "from");
    const auto to = intArg(ctx.call.args, "to");
    if (!from || *from < 1 || *from > n) return error(ErrorCode::NotFound, "no such effect slot");
    if (!to || *to < 1 || *to > n) return error(ErrorCode::OutOfRange, "to out of range");
    if (*from == *to) return {};
    auto slots = readSlots(strip);
    auto sv = std::move(slots[static_cast<std::size_t>(*from - 1)]);
    slots.erase(slots.begin() + (*from - 1));
    slots.insert(slots.begin() + (*to - 1), std::move(sv));
    auto ev = std::move(entries[static_cast<std::size_t>(*from - 1)]);
    entries.erase(entries.begin() + (*from - 1));
    entries.insert(entries.begin() + (*to - 1), std::move(ev));
    writeSlots(strip, slots, ctx.call.id, ctx.client.name);
    updateRack(strip);
    return {};
}

Result FxManager::params(CallContext& ctx) {
    const auto& method = ctx.call.method; // /track/N/fx/M/params
    const auto fp = parseFxPath(method);
    if (!fp || fp->rest != "params" || (fp->strip > 0 && stripOfMethod(method) < 0))
        return error(ErrorCode::NotFound, "no such effect slot");
    auto& entries = strips_[static_cast<std::size_t>(fp->strip)];
    if (fp->slot > static_cast<int>(entries.size())) return error(ErrorCode::NotFound, "no such effect slot");
    auto& e = entries[static_cast<std::size_t>(fp->slot - 1)];
    const auto& args = ctx.call.args;
    const auto offset = std::max<std::int64_t>(0, intArg(args, "offset").value_or(0));
    const auto count = std::clamp<std::int64_t>(intArg(args, "count").value_or(kDefaultParamsCount), 0, kMaxParamsCount);
    const auto keys = splitKeys(strArg(args, "keys").value_or(""));
    const auto slotBase = stripBase(fp->strip) + "/fx/" + std::to_string(fp->slot);
    const auto& tree = server_.store().tree();

    std::string json = "[";
    auto append = [&](const std::string& item) {
        if (json.size() > 1) json += ",";
        json += item;
    };
    if (const auto* spec = findBuiltinFx(e.type)) {
        auto emit = [&](const FxParamSpec& p) {
            const double v = tree.getAs<double>(slotBase + "/param/" + p.key).value_or(p.defaultValue);
            append(paramJson(p.key, p.name, p.unit, p.min, p.max, p.defaultValue, v, builtinText(p, v), p.steps, p.log));
        };
        if (!keys.empty()) {
            for (const auto& k : keys)
                if (const auto* p = findFxParam(*spec, k)) emit(*p);
        } else {
            for (auto i = offset; i < offset + count && i < static_cast<std::int64_t>(spec->params.size()); ++i)
                emit(spec->params[static_cast<std::size_t>(i)]);
        }
    } else if (e.plugin) {
        std::vector<PluginParamInfo> list;
        if (!keys.empty()) {
            for (const auto& k : keys)
                if (auto p = e.plugin->param(k)) list.push_back(*p);
        } else {
            list = e.plugin->params(static_cast<int>(offset), static_cast<int>(count));
        }
        // what a client looked at can be set and followed: put it in the tree
        std::vector<Change> c;
        for (const auto& p : list) {
            append(paramJson(p.key, p.name, p.unit, 0, 1, p.defaultValue, p.value, p.text, p.steps, false));
            if (!e.publishedKeys.count(p.key) && static_cast<int>(e.publishedKeys.size()) < kMaxFxPublishedParams) {
                e.publishedKeys.insert(p.key);
                c.push_back({slotBase + "/param/" + p.key, p.value});
            }
        }
        if (!c.empty()) server_.publishNow(c, {}, ctx.call.id, ctx.client.name);
    } else {
        return error(ErrorCode::Conflict, "the plugin is not loaded");
    }
    json += "]";
    Result r;
    r.value = json;
    return r;
}

Result FxManager::scan(CallContext& ctx) {
    if (!host_) return error(ErrorCode::Internal, "VST3 plugins are not available in this build of the engine");
    const auto dirs = splitDirs(server_.store().tree().getAs<std::string>("/plugins/extra_dirs").value_or(""));
    std::weak_ptr<int> alive = alive_;
    const bool started = host_->scan(
        boolArg(ctx.call.args, "rescan"), dirs,
        [this, alive](PluginHost::ScanProgress p) {
            if (alive.expired()) return;
            asio::post(io_, [this, alive, p] {
                if (alive.expired()) return;
                server_.publishNow({{"/plugins/scan/state", std::string("scanning")},
                                    {"/plugins/scan/progress", std::clamp(p.fraction, 0.0, 1.0)},
                                    {"/plugins/scan/current", p.current}});
            });
        },
        [this, alive] {
            if (alive.expired()) return;
            asio::post(io_, [this, alive] {
                if (alive.expired()) return;
                publishPlugins();
                server_.publishNow({{"/plugins/scan/state", std::string("idle")},
                                    {"/plugins/scan/progress", 1.0},
                                    {"/plugins/scan/current", std::string()}});
                if (verbose_) std::cout << "plugin scan finished: " << publishedPlugins_ << " plugin(s)" << std::endl;
            });
        });
    if (!started) return error(ErrorCode::Conflict, "a scan is already running");
    server_.publishNow({{"/plugins/scan/state", std::string("scanning")}, {"/plugins/scan/progress", 0.0}}, {},
                       ctx.call.id, ctx.client.name);
    return {};
}

void FxManager::publishPlugins() {
    if (!host_) return;
    auto list = host_->plugins();
    std::sort(list.begin(), list.end(), [](const PluginInfo& a, const PluginInfo& b) {
        return a.name != b.name ? a.name < b.name : a.id < b.id;
    });
    std::vector<Change> c;
    std::vector<std::string> removed;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto base = "/plugins/" + std::to_string(i + 1);
        const auto& p = list[i];
        c.push_back({base + "/id", p.id});
        c.push_back({base + "/name", p.name});
        c.push_back({base + "/vendor", p.vendor});
        c.push_back({base + "/category", p.category});
        c.push_back({base + "/instrument", p.instrument});
        c.push_back({base + "/status", p.status});
        c.push_back({base + "/error", p.error});
        c.push_back({base + "/path", p.path});
    }
    for (int i = static_cast<int>(list.size()) + 1; i <= publishedPlugins_; ++i) removed.push_back("/plugins/" + std::to_string(i));
    publishedPlugins_ = static_cast<int>(list.size());
    server_.publishNow(c, removed);
}

// --- plugin callbacks (network thread) ---

void FxManager::onPluginLoaded(std::uint64_t serial, std::shared_ptr<PluginInstance> plugin, const std::string& err) {
    int strip = 0, index = 0;
    Entry* e = findSerial(serial, &strip, &index);
    if (!e) return; // removed while loading; the instance goes away here
    const auto slotBase = stripBase(strip) + "/fx/" + std::to_string(index + 1);
    if (!plugin) {
        server_.publishNow({{slotBase + "/status", std::string("failed")}, {slotBase + "/error", err}});
        if (verbose_) std::cout << "fx load failed " << slotBase << ": " << err << std::endl;
        return;
    }
    const auto& tree = server_.store().tree();
    e->plugin = plugin;
    e->effect = plugin;
    e->slot = std::make_shared<FxSlot>(plugin, tree.getAs<bool>(slotBase + "/bypass").value_or(false));

    std::weak_ptr<int> alive = alive_;
    PluginInstance::Listener l;
    l.onParam = [this, alive, serial](const std::string& key, double value) {
        if (!alive.expired()) queuePluginParam(serial, key, value);
    };
    l.onEditorClosed = [this, alive, serial] {
        if (alive.expired()) return;
        asio::post(io_, [this, alive, serial] {
            if (alive.expired()) return;
            int s = 0, i = 0;
            if (findSerial(serial, &s, &i))
                server_.publishNow({{stripBase(s) + "/fx/" + std::to_string(i + 1) + "/editor", false}});
        });
    };
    plugin->setListener(std::move(l));

    std::vector<Change> c = {{slotBase + "/status", std::string("ok")},
                             {slotBase + "/error", std::string()},
                             {slotBase + "/latency_samples", std::int64_t{plugin->latencySamples()}},
                             {slotBase + "/param_count", std::int64_t{plugin->paramCount()}}};
    for (const auto& p : plugin->params(0, kInitialPublishedParams)) {
        e->publishedKeys.insert(p.key);
        c.push_back({slotBase + "/param/" + p.key, p.value});
    }
    server_.publishNow(c);
    if (tree.getAs<bool>(slotBase + "/editor").value_or(false)) plugin->setEditorOpen(true);
    updateRack(strip);
    if (verbose_) std::cout << "fx loaded " << slotBase << ": " << plugin->name() << ", " << plugin->paramCount() << " params" << std::endl;
}

void FxManager::queuePluginParam(std::uint64_t serial, const std::string& key, double value) {
    // any thread; coalesced so a plugin that changes values continuously cannot flood the clients
    std::lock_guard lock(pendingMutex_);
    pendingParams_[{serial, key}] = value;
    if (flushScheduled_) return;
    flushScheduled_ = true;
    std::weak_ptr<int> alive = alive_;
    asio::post(io_, [this, alive] {
        if (alive.expired()) return;
        flushTimer_.expires_after(std::chrono::milliseconds(20));
        flushTimer_.async_wait([this, alive](asio::error_code ec) {
            if (!ec && !alive.expired()) flushPluginParams();
        });
    });
}

void FxManager::flushPluginParams() {
    std::map<std::pair<std::uint64_t, std::string>, double> pending;
    {
        std::lock_guard lock(pendingMutex_);
        pending.swap(pendingParams_);
        flushScheduled_ = false;
    }
    std::vector<Change> c;
    for (const auto& [id, value] : pending) {
        int s = 0, i = 0;
        Entry* e = findSerial(id.first, &s, &i);
        if (!e) continue;
        if (!e->publishedKeys.count(id.second)) {
            if (static_cast<int>(e->publishedKeys.size()) >= kMaxFxPublishedParams) continue;
            e->publishedKeys.insert(id.second);
        }
        c.push_back({stripBase(s) + "/fx/" + std::to_string(i + 1) + "/param/" + id.second, value});
    }
    if (!c.empty()) server_.publishNow(c);
}

// --- tree -> effects ---

void FxManager::onDelta(const proto::Delta& delta) {
    for (const auto& ch : delta.changes) {
        const auto fp = parseFxPath(ch.path);
        if (!fp || fp->rest.empty()) continue;
        auto& entries = strips_[static_cast<std::size_t>(fp->strip)];
        if (fp->slot > static_cast<int>(entries.size())) continue;
        auto& e = entries[static_cast<std::size_t>(fp->slot - 1)];
        if (fp->rest.rfind("param/", 0) == 0) {
            if (!e.effect) continue;
            if (auto v = asNumber(ch.value)) e.effect->setParam(fp->rest.substr(6), *v);
        } else if (fp->rest == "bypass") {
            if (e.slot) e.slot->setBypass(std::get_if<bool>(&ch.value) && std::get<bool>(ch.value));
        } else if (fp->rest == "editor") {
            if (e.plugin) e.plugin->setEditorOpen(std::get_if<bool>(&ch.value) && std::get<bool>(ch.value));
        }
    }
}

} // namespace dc::mixd
