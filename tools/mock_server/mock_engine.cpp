#include "mock_engine.hpp"

#include "dc/protocol/constants.hpp"
#include "dc/control/video_methods.hpp"
#include "dc/protocol/defaults.hpp"
#include "dc/protocol/video.hpp"
#include "dc/version.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <optional>

namespace dc::mock {

using namespace dc::proto;
using control::CallContext;

namespace {

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

std::string trackBase(int n) { return "/track/" + std::to_string(n); }

} // namespace

MockEngine::MockEngine(asio::io_context& io, control::ControlServer& server, Options options)
    : io_(io), server_(server), options_(options), meterTimer_(io), recordTimer_(io),
      fx_(io, server, &plugins_, nullptr, false), learnTimer_(io) {}

void MockEngine::start() {
    seed();
    registerMethods();
    fx_.start();
    midi_.setDevices({"nanoKONTROL2", "Launchpad Mini"});
    mixd::ControlBindings::Options bo;
    bo.verbose = false;
    bindings_ = std::make_unique<mixd::ControlBindings>(io_, server_, keys_, midi_, bo);
    bindings_->start();
    auto bind = [&](int n, const char* trigger, const char* device, const char* action, const char* target, const char* value) {
        const std::string b = "/control/binding/" + std::to_string(n) + "/";
        server_.publishNow({{b + "trigger", std::string(trigger)}, {b + "device", std::string(device)},
                            {b + "action", std::string(action)}, {b + "target", std::string(target)},
                            {b + "value", std::string(value)}, {b + "enabled", true}, {b + "state", std::string("off")},
                            {b + "error", std::string()}});
    };
    bind(1, "key:ctrl+shift+r", "", "record_toggle", "", "");
    bind(2, "midi:note/1/36", "Launchpad Mini", "pad", "1", "");
    bind(3, "midi:cc/1/0", "nanoKONTROL2", "param", "/track/1/volume_db", "");
    bind(4, "key:ctrl+1", "", "set", "/video/mix/1/program_scene", "1");
    bind(5, "key:f13", "", "toggle", "/track/9/mute", "");
    keys_.setTaken({{proto::ctl::ModCtrl, '1'}}); // binding 4 shows a conflict

    mixd::ProfileManager::Hooks hooks;
    hooks.restoreFx = [this](const auto& slots, const auto& states) { return fx_.restore(slots, states); };
    hooks.restored = [this] { fx_.restored(); };
    hooks.pluginStates = [this] { return fx_.pluginStates(); };
    hooks.deviceNames = [this](const std::vector<std::string>& ids) {
        std::vector<SavedDevice> out;
        const auto& tree = server_.store().tree();
        for (const char* dir : {"input", "output"})
            for (int n = 1; tree.contains("/devices/" + std::string(dir) + "/" + std::to_string(n) + "/id"); ++n) {
                const auto base = "/devices/" + std::string(dir) + "/" + std::to_string(n);
                const auto id = *tree.getAs<std::string>(base + "/id");
                if (std::find(ids.begin(), ids.end(), id) != ids.end())
                    out.push_back({id, tree.getAs<std::string>(base + "/name").value_or(""), dir == std::string("input")});
            }
        return out;
    };
    mixd::ProfileManager::Options po;
    po.directory = options_.profileDir;
    po.statePath = po.directory.empty() ? std::string() : po.directory + "/mixd-profile.txt";
    po.verbose = false;
    profiles_ = std::make_unique<mixd::ProfileManager>(io_, server_, std::move(hooks), po);
    profiles_->start();
    if (!server_.store().tree().contains("/snapshot/1/name")) {
        // something to show: two snapshots of the seeded mixer
        Call c;
        c.method = "/snapshot/save";
        control::ClientInfo self;
        self.name = "mock";
        c.args = {{"name", std::string("トーク")}};
        server_.callLocal(c, self);
        c.args = {{"name", std::string("ゲーム")}};
        server_.callLocal(c, self);
    }
    if (options_.meters) scheduleMeters();
}

void MockEngine::onDelta(const proto::Delta& delta) {
    fx_.onDelta(delta);
    if (bindings_) bindings_->onDelta(delta);
    if (profiles_) profiles_->onDelta(delta);
    for (const auto& c : delta.changes) {
        if (c.path != "/control/learn/binding" || c.value == Value{std::int64_t{0}}) continue;
        // someone presses something on the "engine PC" a moment later
        learnTimer_.expires_after(std::chrono::milliseconds(800));
        learnTimer_.async_wait([this](const asio::error_code& ec) {
            if (ec || stopped_) return;
            if (server_.store().tree().getAs<std::string>("/control/learn/source") == "key") keys_.learn("key:ctrl+alt+f5");
            else midi_.send("nanoKONTROL2", 0xB0, 16, 100);
        });
    }
}

void MockEngine::stop() {
    asio::post(io_, [this] {
        stopped_ = true;
        learnTimer_.cancel();
        if (profiles_) profiles_->stop();
        if (bindings_) bindings_->stop();
        fx_.stop();
        meterTimer_.cancel();
        recordTimer_.cancel();
    });
}

void MockEngine::seed() {
    std::vector<Change> c = globalDefaults();
    auto put = [&](std::string path, Value v) {
        for (auto& x : c)
            if (x.path == path) {
                x.value = std::move(v);
                return;
            }
        c.push_back({std::move(path), std::move(v)});
    };

    put("/system/version", std::string("mock-") + kVersionString);
    put("/system/protocol", std::int64_t{kProtocolVersion});

    struct Dev { const char* dir; const char* id; const char* name; std::int64_t ch; const char* api; };
    const Dev devices[] = {
        {"input", "in-mic", "Mock USB Microphone", 1, "wasapi"},
        {"input", "in-line", "Mock Line In", 2, "wasapi"},
        {"output", "out-spk", "Mock Speakers", 2, "wasapi"},
        {"output", "out-hp", "Mock Headphones", 2, "wasapi"},
        {"output", "vmic-cable", "CABLE Input (VB-Audio Virtual Cable)", 2, "vmic"},
    };
    int inputs = 0, outputs = 0;
    for (const auto& d : devices) {
        const bool in = std::string(d.dir) == "input";
        const int n = in ? ++inputs : ++outputs;
        const std::string base = std::string("/devices/") + d.dir + "/" + std::to_string(n);
        put(base + "/id", std::string(d.id));
        put(base + "/name", std::string(d.name));
        put(base + "/channels", d.ch);
        put(base + "/connected", true);
        put(base + "/api", std::string(d.api));
    }
    put("/devices/process/1/exe", std::string("game.exe"));
    put("/devices/process/1/pid", std::int64_t{4242});
    put("/devices/process/1/title", std::string("Mock Game"));
    put("/devices/process/2/exe", std::string("browser.exe"));
    put("/devices/process/2/pid", std::int64_t{5151});
    put("/devices/process/2/title", std::string("Mock Browser"));

    for (const auto& m : masterDefaults()) c.push_back(m);
    put("/master/out/1/target", std::string("device/out-spk"));
    put("/master/meter_id", std::int64_t{0});

    struct Seed { const char* name; std::int64_t ch; const char* kind; const char* source; };
    const Seed tracks[kInitialTracks] = {
        {"Mic", 1, "device", "in-mic"},
        {"Game", 2, "process", "game.exe"},
        {"Browser", 2, "process", "browser.exe"},
        {"Line", 2, "device", "in-line"},
    };
    for (int n = 1; n <= kInitialTracks; ++n) {
        for (const auto& t : trackDefaults(n)) c.push_back(t);
        const auto& s = tracks[n - 1];
        put(trackBase(n) + "/position", std::int64_t{n});
        put(trackBase(n) + "/name", std::string(s.name));
        put(trackBase(n) + "/channels", s.ch);
        put(trackBase(n) + "/input/kind", std::string(s.kind));
        put(trackBase(n) + "/input/source", std::string(s.source));
        put(trackBase(n) + "/input/state", std::string("ok"));
    }
    put("/mixer/track_count", std::int64_t{kInitialTracks});
    put("/capture/audio/1/source", std::string("master"));
    put("/capture/audio/2/source", std::string("track/1"));
    put("/capture/audio/3/source", std::string("track/2"));

    // what capd would report on a PC with two monitors and a webcam
    put("/video/devices/display/1/id", std::string("mock-display-1"));
    put("/video/devices/display/1/name", std::string("Mock Monitor 27\""));
    put("/video/devices/display/1/width", std::int64_t{2560});
    put("/video/devices/display/1/height", std::int64_t{1440});
    put("/video/devices/display/1/primary", true);
    put("/video/devices/display/2/id", std::string("mock-display-2"));
    put("/video/devices/display/2/name", std::string("Mock Monitor 24\""));
    put("/video/devices/display/2/width", std::int64_t{1920});
    put("/video/devices/display/2/height", std::int64_t{1080});
    put("/video/devices/display/2/primary", false);
    put("/video/devices/window/1/exe", std::string("game.exe"));
    put("/video/devices/window/1/title", std::string("Mock Game"));
    put("/video/devices/window/2/exe", std::string("browser.exe"));
    put("/video/devices/window/2/title", std::string("Mock Browser"));
    put("/video/devices/camera/1/id", std::string("mock-camera-1"));
    put("/video/devices/camera/1/name", std::string("Mock USB Camera"));
    put("/video/source/1/state", std::string("ok"));
    put("/video/source/1/width", std::int64_t{2560});
    put("/video/source/1/height", std::int64_t{1440});

    put("/sampler/library/folder", std::string("C:/Sounds"));
    // a few sampler pads that look loaded, so the pad screen has something to show
    const char* const pads[] = {"Applause", "Airhorn", "Drum roll"};
    for (int i = 0; i < 3; ++i) {
        const std::string base = "/sampler/bank/1/pad/" + std::to_string(i + 1);
        put(base + "/file", std::string("C:/Sounds/") + pads[i] + ".wav");
        put(base + "/state", std::string("ready"));
        put(base + "/duration_ms", std::int64_t{1500 + 500 * i});
    }

    server_.publishNow(c);
    server_.publishNow(videoDerived(server_.store().tree()));
}

void MockEngine::registerMethods() {
    server_.registerMethod("/mixer/track/add", [this](CallContext& ctx) { return addTrack(ctx); });
    server_.registerMethod("/mixer/track/remove", [this](CallContext& ctx) { return removeTrack(ctx); });
    server_.registerMethod("/mixer/track/move", [this](CallContext& ctx) { return moveTrack(ctx); });
    server_.registerMethod("/capture/record/start", [this](CallContext& ctx) { return startRecording(ctx); });
    server_.registerMethod("/capture/record/stop", [this](CallContext& ctx) { return stopRecording(ctx); });
    server_.registerMethod("/capture/report", [this](CallContext& ctx) { return captureReport(ctx); });
    control::registerVideoMethods(server_);

    // recorder and sampler: state only, no audio (protocol.md 5.4.2 / 5.4.3)
    server_.registerMethod("/recorder/start", [this](CallContext& ctx) {
        if (server_.store().tree().getAs<std::string>("/recorder/state") == "recording")
            return error(ErrorCode::Conflict, "already recording");
        const std::string folder = "C:/Users/mock/Music/Desktop Capture/take";
        server_.publishNow({{"/recorder/state", std::string("recording")}, {"/recorder/folder", folder}}, {}, ctx.call.id,
                           ctx.client.name);
        Result r;
        r.value = folder;
        return r;
    });
    server_.registerMethod("/recorder/stop", [this](CallContext& ctx) {
        if (server_.store().tree().getAs<std::string>("/recorder/state") != "recording")
            return error(ErrorCode::Conflict, "not recording");
        server_.publishNow({{"/recorder/state", std::string("idle")}}, {}, ctx.call.id, ctx.client.name);
        return Result{};
    });
    auto padPath = [this](const CallContext& ctx) -> std::string {
        const auto pad = intArg(ctx.call.args, "pad").value_or(0);
        const auto bank = intArg(ctx.call.args, "bank")
                              .value_or(server_.store().tree().getAs<std::int64_t>("/sampler/active_bank").value_or(1));
        if (pad < 1 || pad > kSamplerPads || bank < 1 || bank > kSamplerBanks) return {};
        return "/sampler/bank/" + std::to_string(bank) + "/pad/" + std::to_string(pad) + "/playing";
    };
    server_.registerMethod("/sampler/trigger", [this, padPath](CallContext& ctx) {
        const auto path = padPath(ctx);
        if (path.empty()) return error(ErrorCode::OutOfRange, "pad or bank out of range");
        const bool playing = server_.store().tree().getAs<bool>(path).value_or(false);
        server_.publishNow({{path, !playing}}, {}, ctx.call.id, ctx.client.name);
        return Result{};
    });
    server_.registerMethod("/sampler/release", [this, padPath](CallContext& ctx) {
        const auto path = padPath(ctx);
        if (path.empty()) return error(ErrorCode::OutOfRange, "pad or bank out of range");
        return Result{};
    });
    // a fixed pretend library (no disk access): C:/Sounds with one subfolder
    server_.registerMethod("/sampler/library/list", [this](CallContext& ctx) {
        const auto root = server_.store().tree().getAs<std::string>("/sampler/library/folder").value_or("");
        if (root.empty()) return error(ErrorCode::Conflict, "no sampler library folder set");
        std::string dir;
        if (auto it = ctx.call.args.find("dir"); it != ctx.call.args.end())
            if (auto* v = std::get_if<std::string>(&it->second)) dir = *v;
        std::vector<std::string> folders, files;
        if (dir.empty()) {
            folders = {"Jingles"};
            files = {"Airhorn.wav", "Applause.wav", "Drum roll.wav", "Laugh.flac"};
        } else if (dir == "Jingles") {
            files = {"Intro.mp3", "Outro.ogg"};
        } else {
            return error(ErrorCode::NotFound, "no such folder in the library: " + dir);
        }
        std::string json = "{\"dir\":\"" + dir + "\",\"total\":" + std::to_string(folders.size() + files.size()) +
                           ",\"offset\":0,\"truncated\":false,\"entries\":[";
        bool first = true;
        for (const auto& f : folders) {
            json += std::string(first ? "" : ",") + "{\"name\":\"" + f + "\",\"folder\":true}";
            first = false;
        }
        for (const auto& f : files) {
            json += std::string(first ? "" : ",") + "{\"name\":\"" + f + "\",\"path\":\"" + root + "/" +
                    (dir.empty() ? "" : dir + "/") + f + "\",\"size\":48000}";
            first = false;
        }
        Result r;
        r.value = json + "]}";
        return r;
    });
    server_.registerMethod("/sampler/stop", [this](CallContext& ctx) {
        std::vector<Change> c;
        for (const auto& [path, v] : server_.store().tree().values())
            if (path.rfind("/sampler/", 0) == 0 && path.size() > 8 && path.compare(path.size() - 8, 8, "/playing") == 0 &&
                v == Value{true})
                c.push_back({path, false});
        server_.publishNow(c, {}, ctx.call.id, ctx.client.name);
        return Result{};
    });
}

std::vector<int> MockEngine::tracksInOrder() const {
    std::vector<std::pair<std::int64_t, int>> v;
    const auto& tree = server_.store().tree();
    for (int n = 1; n <= kMaxTracks; ++n)
        if (auto pos = tree.getAs<std::int64_t>(trackBase(n) + "/position")) v.push_back({*pos, n});
    std::sort(v.begin(), v.end());
    std::vector<int> out;
    for (auto& [p, n] : v) out.push_back(n);
    return out;
}

Result MockEngine::addTrack(CallContext& ctx) {
    const auto& args = ctx.call.args;
    const auto& tree = server_.store().tree();
    int id = 0;
    for (int n = 1; n <= kMaxTracks; ++n)
        if (!tree.contains(trackBase(n) + "/position")) {
            id = n;
            break;
        }
    if (id == 0) return error(ErrorCode::LimitReached, "64 tracks already exist");

    const std::int64_t channels = intArg(args, "channels").value_or(2);
    if (channels != 1 && channels != 2) return error(ErrorCode::OutOfRange, "channels must be 1 or 2");

    auto order = tracksInOrder();
    const auto count = static_cast<std::int64_t>(order.size());
    const std::int64_t position = intArg(args, "position").value_or(count + 1);
    if (position < 1 || position > count + 1) return error(ErrorCode::OutOfRange, "position out of range");
    order.insert(order.begin() + (position - 1), id);

    std::vector<Change> c = trackDefaults(id);
    for (auto& x : c) {
        if (x.path == trackBase(id) + "/channels") x.value = channels;
        if (x.path == trackBase(id) + "/name")
            if (auto it = args.find("name"); it != args.end() && std::holds_alternative<std::string>(it->second))
                x.value = it->second;
    }
    for (std::size_t i = 0; i < order.size(); ++i)
        c.push_back({trackBase(order[i]) + "/position", static_cast<std::int64_t>(i + 1)});
    c.push_back({"/mixer/track_count", count + 1});
    server_.publishNow(c, {}, ctx.call.id, ctx.client.name);

    Result r;
    r.value = std::int64_t{id};
    return r;
}

Result MockEngine::removeTrack(CallContext& ctx) {
    const auto id = intArg(ctx.call.args, "track");
    const auto& tree = server_.store().tree();
    if (!id || !tree.contains(trackBase(static_cast<int>(*id)) + "/position"))
        return error(ErrorCode::NotFound, "no such track");
    const std::string ref = "track/" + std::to_string(*id);

    std::vector<Change> c;
    // Clear every target that referenced the removed track.
    for (const auto& [path, v] : tree.values()) {
        const auto* s = std::get_if<std::string>(&v);
        if (!s || *s != ref) continue;
        if (path.size() > 7 && (path.compare(path.size() - 7, 7, "/target") == 0 ||
                                path.compare(path.size() - 7, 7, "/source") == 0))
            c.push_back({path, std::string()});
    }
    auto order = tracksInOrder();
    order.erase(std::remove(order.begin(), order.end(), static_cast<int>(*id)), order.end());
    for (std::size_t i = 0; i < order.size(); ++i)
        c.push_back({trackBase(order[i]) + "/position", static_cast<std::int64_t>(i + 1)});
    c.push_back({"/mixer/track_count", static_cast<std::int64_t>(order.size())});
    server_.publishNow(c, {trackBase(static_cast<int>(*id))}, ctx.call.id, ctx.client.name);
    fx_.clearStrip(static_cast<int>(*id));
    return {};
}

Result MockEngine::moveTrack(CallContext& ctx) {
    const auto id = intArg(ctx.call.args, "track");
    const auto position = intArg(ctx.call.args, "position");
    auto order = tracksInOrder();
    if (!id || std::find(order.begin(), order.end(), static_cast<int>(*id)) == order.end())
        return error(ErrorCode::NotFound, "no such track");
    if (!position || *position < 1 || *position > static_cast<std::int64_t>(order.size()))
        return error(ErrorCode::OutOfRange, "position out of range");
    order.erase(std::remove(order.begin(), order.end(), static_cast<int>(*id)), order.end());
    order.insert(order.begin() + (*position - 1), static_cast<int>(*id));
    std::vector<Change> c;
    for (std::size_t i = 0; i < order.size(); ++i)
        c.push_back({trackBase(order[i]) + "/position", static_cast<std::int64_t>(i + 1)});
    server_.publishNow(c, {}, ctx.call.id, ctx.client.name);
    return {};
}

Result MockEngine::startRecording(CallContext& ctx) {
    const auto& tree = server_.store().tree();
    const auto state = tree.getAs<std::string>("/capture/state").value_or("idle");
    if (state != "idle" && !(options_.delegateCapture && state == "error"))
        return error(ErrorCode::Conflict, "already recording");
    if (tree.getAs<std::int64_t>("/capture/mix").value_or(0) == 0)
        return error(ErrorCode::Conflict, "no video mix assigned to the capture output");

    if (options_.delegateCapture) {
        // capd sees "starting", records, and answers with /capture/report.
        server_.publishNow({{"/capture/state", std::string("starting")}, {"/capture/error", std::string()}}, {},
                           ctx.call.id, ctx.client.name);
        return {};
    }

    char name[64];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(name, sizeof name, "%Y-%m-%d_%H-%M-%S", &tm);
    const auto ext = tree.getAs<std::string>("/capture/container").value_or("mkv");
    const std::string file = std::string("mock://recordings/") + name + "." + ext;

    recordStart_ = std::chrono::steady_clock::now();
    server_.publishNow({{"/capture/state", std::string("recording")},
                        {"/capture/elapsed_ms", std::int64_t{0}},
                        {"/capture/file", file},
                        {"/capture/error", std::string()}},
                       {}, ctx.call.id, ctx.client.name);
    server_.broadcastEventNow({"capture.started", {{"file", file}}});
    scheduleRecordingClock();
    return {};
}

Result MockEngine::stopRecording(CallContext& ctx) {
    const auto& tree = server_.store().tree();
    if (tree.getAs<std::string>("/capture/state") != "recording") return error(ErrorCode::Conflict, "not recording");
    if (options_.delegateCapture) {
        server_.publishNow({{"/capture/state", std::string("stopping")}}, {}, ctx.call.id, ctx.client.name);
        return {};
    }
    recordTimer_.cancel();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                               recordStart_).count();
    const auto file = tree.getAs<std::string>("/capture/file").value_or("");
    server_.publishNow({{"/capture/state", std::string("idle")}, {"/capture/elapsed_ms", std::int64_t{elapsed}}}, {},
                       ctx.call.id, ctx.client.name);
    server_.broadcastEventNow({"capture.stopped", {{"file", file}, {"duration_ms", std::int64_t{elapsed}}}});
    return {};
}

Result MockEngine::captureReport(CallContext& ctx) {
    if (ctx.client.role != ClientRole::Service) return error(ErrorCode::Unauthorized, "capd (service) only");
    const auto& args = ctx.call.args;
    auto str = [&](const char* key) {
        auto it = args.find(key);
        const std::string* v = it == args.end() ? nullptr : std::get_if<std::string>(&it->second);
        return v ? *v : std::string();
    };
    const std::string state = str("state");
    const std::string file = str("file");
    if (state == "recording") {
        recordStart_ = std::chrono::steady_clock::now();
        server_.publishNow({{"/capture/state", std::string("recording")},
                            {"/capture/elapsed_ms", std::int64_t{0}},
                            {"/capture/file", file},
                            {"/capture/error", std::string()}},
                           {}, ctx.call.id, ctx.client.name);
        server_.broadcastEventNow({"capture.started", {{"file", file}}});
        scheduleRecordingClock();
    } else if (state == "idle") {
        recordTimer_.cancel();
        std::int64_t duration = 0;
        if (auto it = args.find("duration_ms"); it != args.end())
            if (auto* d = std::get_if<std::int64_t>(&it->second)) duration = *d;
        server_.publishNow({{"/capture/state", std::string("idle")}, {"/capture/elapsed_ms", duration}}, {},
                           ctx.call.id, ctx.client.name);
        server_.broadcastEventNow({"capture.stopped", {{"file", file}, {"duration_ms", duration}}});
    } else if (state == "error") {
        recordTimer_.cancel();
        server_.publishNow({{"/capture/state", std::string("error")}, {"/capture/error", str("error")}}, {},
                           ctx.call.id, ctx.client.name);
    } else {
        return error(ErrorCode::BadRequest, "state must be recording, idle or error");
    }
    return {};
}

void MockEngine::scheduleRecordingClock() {
    recordTimer_.expires_after(std::chrono::milliseconds(250));
    recordTimer_.async_wait([this](asio::error_code ec) {
        if (ec || stopped_) return;
        if (server_.store().tree().getAs<std::string>("/capture/state") != "recording") return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - recordStart_).count();
        server_.publishNow({{"/capture/elapsed_ms", std::int64_t{elapsed}}});
        scheduleRecordingClock();
    });
}

void MockEngine::scheduleMeters() {
    meterTimer_.expires_after(options_.meterInterval);
    meterTimer_.async_wait([this](asio::error_code ec) {
        if (ec || stopped_) return;
        phase_ += 0.05;
        const auto& tree = server_.store().tree();
        MeterPacket p;
        p.seq = meterSeq_++;
        p.timeUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());

        auto entry = [&](const std::string& base, int index) {
            MeterEntry e;
            e.meterId = static_cast<std::uint16_t>(tree.getAs<std::int64_t>(base + "/meter_id").value_or(0));
            e.channels = static_cast<std::uint8_t>(tree.getAs<std::int64_t>(base + "/channels").value_or(2));
            const bool muted = tree.getAs<bool>(base + "/mute").value_or(false);
            const double vol = tree.getAs<double>(base + "/volume_db").value_or(0.0);
            for (int ch = 0; ch < e.channels; ++ch) {
                const double wobble = 0.5 + 0.5 * std::sin(phase_ * (1.0 + index * 0.3) + ch);
                const double db = muted ? -144.0 : std::max(-144.0, -40.0 + 34.0 * wobble + vol);
                const float amp = static_cast<float>(std::pow(10.0, db / 20.0));
                e.peakCdb[ch] = linearToCdb(amp);
                e.rmsCdb[ch] = linearToCdb(amp * 0.5f);
            }
            p.entries.push_back(e);
        };
        entry("/master", 0);
        for (int n : tracksInOrder()) entry(trackBase(n), n);
        server_.sendMeters(std::move(p));
        scheduleMeters();
    });
}

} // namespace dc::mock
