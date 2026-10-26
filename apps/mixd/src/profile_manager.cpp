#include "profile_manager.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace dc::mixd {

using namespace dc::proto;
using control::CallContext;

namespace {

constexpr const char* kExtension = ".dcprofile";
constexpr const char* kFirstProfile = "標準";

// paths are UTF-8 (%APPDATA% may hold any characters)
std::filesystem::path fsPath(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

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

std::string lowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string settingsDir() {
#ifdef _WIN32
    if (const wchar_t* a = _wgetenv(L"APPDATA")) return utf8Of(std::filesystem::path(a) / L"DesktopCapture");
    return ".";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/desktop-capture";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.config/desktop-capture";
    return ".";
#endif
}

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream in(fsPath(path), std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Writes path through a temporary file; the old content goes to backup when it is not empty.
bool writeAtomically(const std::string& path, const std::string& text, const std::string& backup, std::string* why) {
    std::error_code ec;
    const auto target = fsPath(path);
    std::filesystem::create_directories(target.parent_path(), ec);
    const auto tmp = fsPath(path + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) {
            if (why) *why = "書き込めない: " + path;
            return false;
        }
    }
    if (!backup.empty() && std::filesystem::exists(target, ec)) std::filesystem::rename(target, fsPath(backup), ec);
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        if (why) *why = "置き換えられない: " + path + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

std::int64_t now() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::string snapshotBase(int n) { return "/snapshot/" + std::to_string(n); }

// a change of something a profile keeps
bool kept(const Schema& schema, const std::string& path) {
    if (path.rfind("/snapshot/", 0) == 0) return path != "/snapshot/last";
    const ParamSpec* spec = schema.find(path);
    return spec && inProfile(*spec);
}
bool keptSubtree(const std::string& removed) {
    for (const char* p : {"/track/", "/master/fx", "/video/source/", "/video/scene/", "/video/mix/", "/snapshot/"})
        if (removed.rfind(p, 0) == 0) return true;
    return false;
}

} // namespace

ProfileManager::ProfileManager(asio::io_context& io, control::ControlServer& server, Hooks hooks, Options options)
    : io_(io), server_(server), hooks_(std::move(hooks)), options_(std::move(options)), saveTimer_(io) {}

ProfileManager::~ProfileManager() = default;

std::string ProfileManager::defaultDirectory() { return utf8Of(fsPath(settingsDir()) / "profiles"); }
std::string ProfileManager::defaultStatePath() { return utf8Of(fsPath(settingsDir()) / "mixd-profile.txt"); }

void ProfileManager::start() {
    initial_ = profileValues(server_.store().tree());
    auto reg = [this](const char* method, Result (ProfileManager::*fn)(CallContext&)) {
        server_.registerMethod(method, [this, fn](CallContext& ctx) { return (this->*fn)(ctx); });
    };
    reg("/profile/save", &ProfileManager::save);
    reg("/profile/load", &ProfileManager::load);
    reg("/profile/new", &ProfileManager::create);
    reg("/profile/rename", &ProfileManager::rename);
    reg("/profile/remove", &ProfileManager::remove);
    reg("/snapshot/save", &ProfileManager::snapshotSave);
    reg("/snapshot/recall", &ProfileManager::snapshotRecall);
    reg("/snapshot/remove", &ProfileManager::snapshotRemove);

    readState();
    applying_ = true;
    server_.publishNow({{"/profile/directory", options_.directory},
                        {"/profile/autosave", autosave_},
                        {"/plugins/extra_dirs", extraDirs_}});
    applying_ = false;
    if (!enabled()) return;
    refreshList();
    std::string name = !options_.startProfile.empty() ? options_.startProfile : current_;
    if (name.empty() || !isValidProfileName(name)) name = kFirstProfile;
    if (const Listed* l = findListed(name)) name = l->name;
    current_ = name;
    if (!findListed(name)) {
        // the first start (or a new name from --profile): the fresh engine, saved on the first change
        applying_ = true;
        server_.publishNow({{"/profile/current", current_}});
        applying_ = false;
        if (options_.verbose) std::cout << "profile: " << current_ << " (new)" << std::endl;
        writeState();
        return;
    }
    std::string why;
    bool fromBackup = false;
    auto data = readProfile(name, &why, &fromBackup);
    if (!data) {
        // keep the broken file for a look, start fresh under the same name
        std::error_code ec;
        std::filesystem::rename(fsPath(fileOf(name)), fsPath(fileOf(name) + ".broken"), ec);
        applying_ = true;
        server_.publishNow({{"/profile/current", current_}});
        applying_ = false;
        setError(name + " を読めなかったので初期状態で始めた（" + why + "）");
        if (options_.verbose) std::cout << "profile: " << name << " unreadable (" << why << "), starting fresh" << std::endl;
        refreshList();
        writeState();
        return;
    }
    apply(*data, name, 0, {});
    if (fromBackup) setError(name + " が読めなかったので 1 つ前の版（.bak）を読んだ（" + why + "）");
    writeState();
}

void ProfileManager::stop() {
    if (stopped_) return;
    saveTimer_.cancel();
    if (modified_ && autosave_ && enabled()) saveCurrent();
    stopped_ = true;
}

// --- files ---

std::string ProfileManager::fileOf(const std::string& name) const {
    return utf8Of(fsPath(options_.directory) / fsPath(name + kExtension));
}

const ProfileManager::Listed* ProfileManager::findListed(const std::string& name) const {
    const auto key = lowerAscii(name);
    for (const auto& l : list_)
        if (lowerAscii(l.name) == key) return &l;
    return nullptr;
}

void ProfileManager::refreshList() {
    if (!enabled()) return;
    std::vector<Listed> found;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(fsPath(options_.directory), ec), end; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        if (!it->is_regular_file(ec) || p.extension() != kExtension) continue;
        const auto name = utf8Of(p.stem());
        if (!isValidProfileName(name)) continue;
        const auto t = std::filesystem::last_write_time(p, ec);
        const auto sys = std::chrono::system_clock::now() +
                         std::chrono::duration_cast<std::chrono::system_clock::duration>(t - std::filesystem::file_time_type::clock::now());
        found.push_back({name, static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(sys))});
        if (found.size() >= static_cast<std::size_t>(kMaxProfiles)) break;
    }
    std::sort(found.begin(), found.end(), [](const Listed& a, const Listed& b) { return a.name < b.name; });
    const bool same = found.size() == list_.size() &&
                      std::equal(found.begin(), found.end(), list_.begin(),
                                 [](const Listed& a, const Listed& b) { return a.name == b.name && a.modified == b.modified; });
    if (same && server_.store().tree().contains("/profile/list/1/name") == !found.empty()) return;
    list_ = std::move(found);
    std::vector<Change> c;
    for (std::size_t i = 0; i < list_.size(); ++i) {
        const auto base = "/profile/list/" + std::to_string(i + 1);
        c.push_back({base + "/name", list_[i].name});
        c.push_back({base + "/modified", list_[i].modified});
    }
    applying_ = true;
    server_.publishNow(c, {"/profile/list"});
    applying_ = false;
}

bool ProfileManager::writeProfile(const std::string& name, std::string* why) {
    const auto& tree = server_.store().tree();
    ProfileData data;
    data.values = profileValues(tree);
    if (hooks_.deviceNames) data.devices = hooks_.deviceNames(deviceRefs(tree));
    if (hooks_.pluginStates) data.fxState = hooks_.pluginStates();
    for (const auto& [n, s] : snapshots_) data.snapshots.push_back(s);
    const auto path = fileOf(name);
    return writeAtomically(path, proto::writeProfile(data), path + ".bak", why);
}

std::optional<ProfileData> ProfileManager::readProfile(const std::string& name, std::string* why, bool* fromBackup) {
    *fromBackup = false;
    const auto path = fileOf(name);
    std::string first;
    if (auto text = readFile(path)) {
        if (auto p = proto::readProfile(*text, &first)) return p;
    } else {
        first = "開けない";
    }
    std::string second;
    if (auto text = readFile(path + ".bak")) {
        if (auto p = proto::readProfile(*text, &second)) {
            *fromBackup = true;
            *why = first;
            return p;
        }
    }
    *why = first;
    return std::nullopt;
}

bool ProfileManager::saveCurrent() {
    if (!enabled() || current_.empty()) return false;
    saveTimer_.cancel();
    firstUnsaved_.reset();
    std::string why;
    if (!writeProfile(current_, &why)) {
        setError(why);
        if (options_.verbose) std::cout << "profile: save failed: " << why << std::endl;
        return false;
    }
    modified_ = false;
    applying_ = true;
    server_.publishNow({{"/profile/modified", false}, {"/profile/error", std::string()}});
    applying_ = false;
    if (options_.verbose) std::cout << "profile: saved " << current_ << std::endl;
    refreshList();
    return true;
}

void ProfileManager::setError(const std::string& e) {
    applying_ = true;
    server_.publishNow({{"/profile/error", e}});
    applying_ = false;
}

void ProfileManager::readState() {
    if (options_.statePath.empty()) return;
    std::ifstream in(fsPath(options_.statePath));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const auto key = line.substr(0, tab), value = line.substr(tab + 1);
        if (key == "current") current_ = value;
        else if (key == "autosave") autosave_ = value != "0";
        else if (key == "extra_dirs") extraDirs_ = value;
    }
}

void ProfileManager::writeState() {
    if (options_.statePath.empty()) return;
    auto clean = [](std::string s) {
        for (auto& c : s)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    };
    std::string why;
    if (!writeAtomically(options_.statePath,
                         "current\t" + clean(current_) + "\nautosave\t" + (autosave_ ? "1" : "0") + "\nextra_dirs\t" +
                             clean(extraDirs_) + "\n",
                         {}, &why) &&
        options_.verbose)
        std::cout << "profile: " << why << std::endl;
}

// --- restore ---

void ProfileManager::apply(const ProfileData& data, const std::string& name, std::uint64_t originId,
                           const std::string& origin) {
    const auto& tree = server_.store().tree();
    const auto plan = planRestore(tree, data.values);
    if (hooks_.rememberDevices) hooks_.rememberDevices(data.devices); // before the plan rebuild looks for them
    std::vector<Change> c = plan.changes;
    if (hooks_.restoreFx)
        for (auto& f : hooks_.restoreFx(plan.fx, data.fxState)) c.push_back(std::move(f));
    snapshots_.clear();
    for (const auto& s : data.snapshots) {
        if (s.number < 1 || s.number > kMaxSnapshots) continue;
        snapshots_[s.number] = s;
        c.push_back({snapshotBase(s.number) + "/name", s.name});
        c.push_back({snapshotBase(s.number) + "/modified", s.modified});
    }
    current_ = name;
    modified_ = false;
    firstUnsaved_.reset();
    saveTimer_.cancel();
    c.push_back({"/profile/current", current_});
    c.push_back({"/profile/modified", false});
    c.push_back({"/profile/error", std::string()});
    c.push_back({"/snapshot/last", std::int64_t{0}});
    auto removed = plan.removed;
    removed.push_back("/snapshot");
    applying_ = true;
    server_.publishNow(c, removed, originId, origin);
    applying_ = false;
    if (hooks_.restored) hooks_.restored();
    Event ev;
    ev.name = "profile.loaded";
    ev.data = {{"name", name}, {"skipped", std::int64_t{plan.skipped}}};
    server_.broadcastEventNow(ev);
    if (options_.verbose)
        std::cout << "profile: loaded " << name << (plan.skipped ? " (" + std::to_string(plan.skipped) + " values skipped)" : "")
                  << std::endl;
}

std::optional<std::string> ProfileManager::busyReason() const {
    const auto& tree = server_.store().tree();
    const auto capture = tree.getAs<std::string>("/capture/state").value_or("idle");
    if (capture != "idle" && capture != "error") return "録画中は切り替えられない";
    if (tree.getAs<std::string>("/recorder/state") == "recording") return "録音中は切り替えられない";
    return std::nullopt;
}

// --- autosave ---

void ProfileManager::onDelta(const Delta& delta) {
    if (applying_ || stopped_) return;
    const auto& schema = server_.store().schema();
    bool changed = false, soon = false, state = false;
    for (const auto& r : delta.removed) changed = changed || keptSubtree(r);
    for (const auto& c : delta.changes) {
        if (c.path == "/profile/autosave") {
            if (const auto* b = std::get_if<bool>(&c.value)) autosave_ = *b;
            state = true;
            if (autosave_ && modified_) soon = true;
        } else if (c.path == "/plugins/extra_dirs") {
            if (const auto* s = std::get_if<std::string>(&c.value)) extraDirs_ = *s;
            state = true;
        } else if (c.path.size() > 7 && c.path.compare(c.path.size() - 7, 7, "/editor") == 0 && c.value == Value{false}) {
            // a plugin window closed: what changed inside it may be only in the plugin's state
            changed = true;
            soon = true;
        } else if (c.path.rfind("/snapshot/", 0) == 0 && c.path.size() > 5 && c.path.compare(c.path.size() - 5, 5, "/name") == 0) {
            const int n = std::atoi(c.path.c_str() + 10);
            if (auto it = snapshots_.find(n); it != snapshots_.end())
                if (const auto* s = std::get_if<std::string>(&c.value)) it->second.name = *s;
            changed = true;
        } else {
            changed = changed || kept(schema, c.path);
        }
    }
    if (state) asio::post(io_, [this] {
        if (!stopped_) writeState();
    });
    if (changed) markModified();
    if (soon && modified_ && autosave_) scheduleSave(std::chrono::milliseconds(0));
}

void ProfileManager::markModified() {
    if (!modified_) {
        modified_ = true;
        asio::post(io_, [this] {
            if (stopped_ || !modified_) return;
            applying_ = true;
            server_.publishNow({{"/profile/modified", true}});
            applying_ = false;
        });
    }
    if (autosave_ && enabled()) scheduleSave(options_.autosaveDelay);
}

void ProfileManager::scheduleSave(std::chrono::milliseconds delay) {
    const auto t = std::chrono::steady_clock::now();
    if (!firstUnsaved_) firstUnsaved_ = t;
    auto due = t + delay;
    due = std::min(due, *firstUnsaved_ + options_.autosaveMaxDelay);
    saveTimer_.expires_at(due);
    saveTimer_.async_wait([this](const asio::error_code& ec) {
        if (ec || stopped_ || !modified_ || !autosave_) return;
        saveCurrent();
    });
}

// --- methods ---

Result ProfileManager::save(CallContext& ctx) {
    if (!enabled()) return error(ErrorCode::Conflict, "profiles are off in this engine");
    const auto name = strArg(ctx.call.args, "name");
    if (name && !isValidProfileName(*name)) return error(ErrorCode::BadRequest, "その名前はファイル名に使えない");
    if (name) {
        const Listed* l = findListed(*name);
        current_ = l ? l->name : *name;
    }
    if (!saveCurrent()) return error(ErrorCode::Internal, server_.store().tree().getAs<std::string>("/profile/error").value_or(""));
    server_.publishNow({{"/profile/current", current_}}, {}, ctx.call.id, ctx.client.name);
    writeState();
    Result r;
    r.value = current_;
    return r;
}

Result ProfileManager::load(CallContext& ctx) {
    if (!enabled()) return error(ErrorCode::Conflict, "profiles are off in this engine");
    const auto name = strArg(ctx.call.args, "name");
    if (!name) return error(ErrorCode::BadRequest, "name is required");
    if (const auto busy = busyReason()) return error(ErrorCode::Conflict, *busy);
    refreshList();
    const Listed* l = findListed(*name);
    if (!l) return error(ErrorCode::NotFound, "そのプロファイルがない");
    const std::string target = l->name;
    std::string why;
    bool fromBackup = false;
    // read first: a profile that cannot be read leaves everything as it is
    auto data = readProfile(target, &why, &fromBackup);
    if (!data) return error(ErrorCode::Internal, target + " を読めない（" + why + "）");
    if (autosave_ && target != current_) saveCurrent();
    apply(*data, target, ctx.call.id, ctx.client.name);
    if (fromBackup) setError(target + " が読めなかったので 1 つ前の版（.bak）を読んだ（" + why + "）");
    writeState();
    return {};
}

Result ProfileManager::create(CallContext& ctx) {
    if (!enabled()) return error(ErrorCode::Conflict, "profiles are off in this engine");
    const auto name = strArg(ctx.call.args, "name");
    if (!name) return error(ErrorCode::BadRequest, "name is required");
    if (!isValidProfileName(*name)) return error(ErrorCode::BadRequest, "その名前はファイル名に使えない");
    if (const auto busy = busyReason()) return error(ErrorCode::Conflict, *busy);
    refreshList();
    if (findListed(*name)) return error(ErrorCode::Conflict, "同じ名前のプロファイルがある");
    if (autosave_) saveCurrent();
    ProfileData fresh;
    fresh.values = initial_;
    apply(fresh, *name, ctx.call.id, ctx.client.name);
    saveCurrent();
    writeState();
    return {};
}

Result ProfileManager::rename(CallContext& ctx) {
    if (!enabled()) return error(ErrorCode::Conflict, "profiles are off in this engine");
    const auto name = strArg(ctx.call.args, "name");
    const auto to = strArg(ctx.call.args, "new_name");
    if (!name || !to) return error(ErrorCode::BadRequest, "name and new_name are required");
    if (!isValidProfileName(*to)) return error(ErrorCode::BadRequest, "その名前はファイル名に使えない");
    refreshList();
    const Listed* l = findListed(*name);
    const bool isCurrent = lowerAscii(*name) == lowerAscii(current_);
    if (!l && !isCurrent) return error(ErrorCode::NotFound, "そのプロファイルがない");
    const std::string from = l ? l->name : current_;
    if (const Listed* other = findListed(*to); other && lowerAscii(other->name) != lowerAscii(from))
        return error(ErrorCode::Conflict, "同じ名前のプロファイルがある");
    if (l) {
        std::error_code ec;
        // through a temporary name so that a change of case only works on Windows too
        const auto tmp = fsPath(fileOf(from) + ".renaming");
        std::filesystem::rename(fsPath(fileOf(from)), tmp, ec);
        if (!ec) std::filesystem::rename(tmp, fsPath(fileOf(*to)), ec);
        if (ec) return error(ErrorCode::Internal, "名前を変えられない（" + ec.message() + "）");
        std::filesystem::remove(fsPath(fileOf(from) + ".bak"), ec);
    }
    if (isCurrent) {
        current_ = *to;
        server_.publishNow({{"/profile/current", current_}}, {}, ctx.call.id, ctx.client.name);
        writeState();
    }
    refreshList();
    return {};
}

Result ProfileManager::remove(CallContext& ctx) {
    if (!enabled()) return error(ErrorCode::Conflict, "profiles are off in this engine");
    const auto name = strArg(ctx.call.args, "name");
    if (!name) return error(ErrorCode::BadRequest, "name is required");
    if (lowerAscii(*name) == lowerAscii(current_)) return error(ErrorCode::Conflict, "使用中のプロファイルは消せない");
    refreshList();
    const Listed* l = findListed(*name);
    if (!l) return error(ErrorCode::NotFound, "そのプロファイルがない");
    std::error_code ec;
    std::filesystem::remove(fsPath(fileOf(l->name)), ec);
    if (ec) return error(ErrorCode::Internal, "消せない（" + ec.message() + "）");
    std::filesystem::remove(fsPath(fileOf(l->name) + ".bak"), ec);
    refreshList();
    return {};
}

Result ProfileManager::snapshotSave(CallContext& ctx) {
    const auto name = strArg(ctx.call.args, "name");
    int n = 0;
    if (const auto given = intArg(ctx.call.args, "snapshot")) {
        if (!snapshots_.count(static_cast<int>(*given))) return error(ErrorCode::NotFound, "no such snapshot");
        n = static_cast<int>(*given);
    } else {
        n = 1;
        while (n <= kMaxSnapshots && snapshots_.count(n)) ++n;
        if (n > kMaxSnapshots) return error(ErrorCode::LimitReached, "64 snapshots already");
    }
    auto& s = snapshots_[n];
    s.number = n;
    if (name) s.name = *name;
    else if (s.name.empty()) s.name = "Snapshot " + std::to_string(n);
    s.modified = now();
    s.values = snapshotValues(server_.store().tree());
    server_.publishNow({{snapshotBase(n) + "/name", s.name}, {snapshotBase(n) + "/modified", s.modified},
                        {"/snapshot/last", std::int64_t{n}}},
                       {}, ctx.call.id, ctx.client.name);
    markModified();
    Result r;
    r.value = std::int64_t{n};
    return r;
}

Result ProfileManager::snapshotRecall(CallContext& ctx) {
    const auto n = intArg(ctx.call.args, "snapshot");
    const auto it = n ? snapshots_.find(static_cast<int>(*n)) : snapshots_.end();
    if (it == snapshots_.end()) return error(ErrorCode::NotFound, "no such snapshot");
    Set set;
    set.id = ctx.call.id;
    set.changes = planRecall(server_.store().tree(), it->second.values);
    if (!set.changes.empty()) {
        const Result r = server_.applyLocalSet(set, ctx.client.name);
        if (r.code != ErrorCode::Ok) return r;
    }
    server_.publishNow({{"/snapshot/last", std::int64_t{it->first}}}, {}, ctx.call.id, ctx.client.name);
    if (options_.verbose) std::cout << "snapshot: recalled " << it->first << " " << it->second.name << std::endl;
    return {};
}

Result ProfileManager::snapshotRemove(CallContext& ctx) {
    const auto n = intArg(ctx.call.args, "snapshot");
    const auto it = n ? snapshots_.find(static_cast<int>(*n)) : snapshots_.end();
    if (it == snapshots_.end()) return error(ErrorCode::NotFound, "no such snapshot");
    const int number = it->first;
    snapshots_.erase(it);
    std::vector<Change> c;
    if (server_.store().tree().getAs<std::int64_t>("/snapshot/last") == number) c.push_back({"/snapshot/last", std::int64_t{0}});
    server_.publishNow(c, {snapshotBase(number)}, ctx.call.id, ctx.client.name);
    return {};
}

} // namespace dc::mixd
