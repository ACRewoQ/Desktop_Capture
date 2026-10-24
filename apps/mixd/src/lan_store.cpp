#include "lan_store.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace dc::mixd {

namespace {

// paths are UTF-8 (%APPDATA% may hold any characters)
std::filesystem::path fsPath(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

// names may contain anything but tabs and newlines
std::string clean(std::string s) {
    for (auto& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

} // namespace

std::string randomHex(std::size_t bytes) {
    static std::random_device rd;
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto b = static_cast<unsigned>(rd() & 0xFF);
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

LanStore::LanStore(std::string path) : path_(std::move(path)) {}

std::string LanStore::defaultPath() {
#ifdef _WIN32
    if (const wchar_t* a = _wgetenv(L"APPDATA")) {
        const auto u8 = (std::filesystem::path(a) / L"DesktopCapture" / L"mixd-lan.txt").u8string();
        return std::string(u8.begin(), u8.end());
    }
    return "mixd-lan.txt";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/desktop-capture/mixd-lan.txt";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.config/desktop-capture/mixd-lan.txt";
    return "mixd-lan.txt";
#endif
}

void LanStore::load() {
    if (!path_.empty()) {
        std::ifstream in(fsPath(path_));
        std::string line;
        while (std::getline(in, line)) {
            const auto f = splitTabs(line);
            if (f.empty()) continue;
            if (f[0] == "server_id" && f.size() >= 2) serverId_ = f[1];
            else if (f[0] == "paired" && f.size() >= 5)
                paired_.push_back({f[1], f[2], f[3] == "peer" ? proto::ClientRole::Peer : proto::ClientRole::Remote, f[4]});
            else if (f[0] == "peer_token" && f.size() >= 3) peerTokens_[f[1]] = f[2];
            else if (f[0] == "manual_peer" && f.size() >= 2) manual_.push_back({f[1], f.size() >= 3 ? f[2] : ""});
        }
    }
    if (serverId_.empty()) {
        serverId_ = randomHex(8);
        save();
    }
}

void LanStore::save() const {
    if (path_.empty()) return;
    const auto p = fsPath(path_);
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "# mixd LAN settings (server id, pairings). Written by mixd.\n";
        out << "server_id\t" << serverId_ << "\n";
        for (const auto& c : paired_)
            out << "paired\t" << clean(c.id) << "\t" << clean(c.name) << "\t"
                << (c.role == proto::ClientRole::Peer ? "peer" : "remote") << "\t" << c.token << "\n";
        for (const auto& [id, token] : peerTokens_) out << "peer_token\t" << clean(id) << "\t" << token << "\n";
        for (const auto& m : manual_) out << "manual_peer\t" << clean(m.address) << "\t" << clean(m.id) << "\n";
    }
    std::filesystem::rename(tmp, p, ec);
}

const PairedClient* LanStore::findByToken(const std::string& token) const {
    if (token.empty()) return nullptr;
    for (const auto& c : paired_)
        if (c.token == token) return &c;
    return nullptr;
}

std::string LanStore::addPaired(const std::string& id, const std::string& name, proto::ClientRole role) {
    removePaired(id);
    PairedClient c{id, name, role, randomHex(16)};
    paired_.push_back(c);
    save();
    return c.token;
}

bool LanStore::removePaired(const std::string& id) {
    const auto before = paired_.size();
    paired_.erase(std::remove_if(paired_.begin(), paired_.end(), [&](const PairedClient& c) { return c.id == id; }),
                  paired_.end());
    if (paired_.size() == before) return false;
    save();
    return true;
}

void LanStore::setPeerToken(const std::string& peerId, const std::string& token) {
    peerTokens_[peerId] = token;
    save();
}

void LanStore::removePeerToken(const std::string& peerId) {
    if (peerTokens_.erase(peerId)) save();
}

void LanStore::addManualPeer(const std::string& address) {
    for (const auto& m : manual_)
        if (m.address == address) return;
    manual_.push_back({address, ""});
    save();
}

void LanStore::setManualPeerId(const std::string& address, const std::string& id) {
    for (auto& m : manual_)
        if (m.address == address && m.id != id) {
            m.id = id;
            save();
        }
}

void LanStore::removeManualPeer(const std::string& address) {
    const auto before = manual_.size();
    manual_.erase(std::remove_if(manual_.begin(), manual_.end(), [&](const ManualPeer& m) { return m.address == address; }),
                  manual_.end());
    if (manual_.size() != before) save();
}

std::string PairingWindow::start(double now) {
    static std::random_device rd;
    std::uniform_int_distribution<int> digit(0, 9);
    code_.clear();
    for (int i = 0; i < 6; ++i) code_ += static_cast<char>('0' + digit(rd));
    active_ = true;
    until_ = now + kSeconds;
    failures_ = 0;
    return code_;
}

PairingWindow::Check PairingWindow::check(const std::string& code, double now) {
    if (!active(now)) return Check::Closed;
    if (code == code_) {
        active_ = false;
        return Check::Ok;
    }
    if (++failures_ >= kMaxFailures) active_ = false;
    return Check::Wrong;
}

} // namespace dc::mixd
