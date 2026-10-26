#include "tray_core.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <system_error>

namespace dc::tray {

std::int64_t RestartTracker::onExit(std::int64_t uptimeMs) {
    if (uptimeMs >= policy_.stableMs) failures_ = 0;
    ++failures_;
    if (failures_ > policy_.maxFailures || policy_.delaysMs.empty()) return -1;
    const size_t i = std::min(static_cast<size_t>(failures_ - 1), policy_.delaysMs.size() - 1);
    return policy_.delaysMs[i];
}

std::string quoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    // Backslashes are literal unless they come before a quote; before a quote (ours or one in the text) they
    // are doubled.
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
        } else {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

std::string buildCommandLine(const std::string& exe, const std::vector<std::string>& args, const std::string& extra) {
    std::string line = quoteArg(exe);
    for (const auto& a : args) line += " " + quoteArg(a);
    const auto first = extra.find_first_not_of(" \t");
    if (first != std::string::npos) line += " " + extra.substr(first);
    return line;
}

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

TrayConfig parseConfig(const std::string& text, std::vector<std::string>* errors) {
    TrayConfig c;
    std::istringstream in(text);
    std::string line;
    int number = 0;
    auto error = [&](const std::string& what) {
        if (errors) errors->push_back("line " + std::to_string(number) + ": " + what);
    };
    while (std::getline(in, line)) {
        ++number;
        if (number == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // Notepad's UTF-8 BOM
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) {
            error("no '=' in \"" + t + "\"");
            continue;
        }
        const std::string key = trim(t.substr(0, eq));
        const std::string value = trim(t.substr(eq + 1));
        if (key == "mixd_args") c.mixdArgs = value;
        else if (key == "capd_args") c.capdArgs = value;
        else if (key == "ui_args") c.uiArgs = value;
        else if (key == "capd") {
            if (value == "on") c.capd = true;
            else if (value == "off") c.capd = false;
            else error("capd must be on or off");
        } else {
            error("unknown key \"" + key + "\"");
        }
    }
    return c;
}

bool startupApproved(const std::vector<std::uint8_t>& value) { return value.empty() || (value[0] & 1) == 0; }

void rotateLogs(const std::filesystem::path& dir, const std::string& name, int keep) {
    namespace fs = std::filesystem;
    auto file = [&](int i) { return dir / (i == 0 ? name + ".log" : name + "." + std::to_string(i) + ".log"); };
    std::error_code ec;
    if (keep < 1) keep = 1;
    fs::remove(file(keep - 1), ec);
    for (int i = keep - 2; i >= 0; --i)
        if (fs::exists(file(i), ec)) fs::rename(file(i), file(i + 1), ec);
}

const char* stateKey(ChildState s) {
    switch (s) {
    case ChildState::Stopped: return "stopped";
    case ChildState::Starting: return "starting";
    case ChildState::Running: return "running";
    case ChildState::Restarting: return "restarting";
    case ChildState::Failed: return "failed";
    case ChildState::External: return "external";
    }
    return "?";
}

const char* stateLabel(ChildState s) {
    switch (s) {
    case ChildState::Stopped: return "停止";
    case ChildState::Starting: return "起動中";
    case ChildState::Running: return "動作中";
    case ChildState::Restarting: return "立ち上げ直し中";
    case ChildState::Failed: return "起動できません";
    case ChildState::External: return "動作中（別に起動されたもの）";
    }
    return "?";
}

std::string describeExit(std::uint32_t code) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned>(code));
    switch (code) {
    case 0xC0000005u: return std::string("異常終了（") + hex + " アクセス違反）";
    case 0xC0000409u: return std::string("異常終了（") + hex + " スタックの破損・強制終了）";
    case 0xC00000FDu: return std::string("異常終了（") + hex + " スタック オーバーフロー）";
    case 0xC0000374u: return std::string("異常終了（") + hex + " ヒープの破損）";
    case 0xC000013Au: return std::string("Ctrl+C で終了（") + hex + "）";
    case 0xE06D7363u: return std::string("異常終了（") + hex + " 処理されない C++ 例外）";
    default: break;
    }
    if (code >= 0xC0000000u) return std::string("異常終了（") + hex + "）";
    return "終了コード " + std::to_string(code);
}

std::string formatStatus(std::uint32_t trayPid, const std::vector<ChildStatus>& children) {
    std::string out = "tray_pid=" + std::to_string(trayPid) + "\n";
    for (const auto& c : children) {
        out += c.name + ".state=" + stateKey(c.state) + "\n";
        out += c.name + ".pid=" + std::to_string(c.pid) + "\n";
        out += c.name + ".restarts=" + std::to_string(c.restarts) + "\n";
        if (c.hasExit) {
            char hex[16];
            std::snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned>(c.lastExit));
            out += c.name + ".last_exit=" + hex + "\n";
        }
    }
    return out;
}

Health overallHealth(const std::vector<ChildStatus>& children) {
    bool busy = false, any = false;
    for (const auto& c : children) {
        if (c.state == ChildState::Failed) return Health::Failed;
        if (c.state == ChildState::Starting || c.state == ChildState::Restarting) busy = true;
        if (c.state != ChildState::Stopped) any = true;
    }
    if (busy) return Health::Busy;
    return any ? Health::Ok : Health::Stopped;
}

} // namespace dc::tray
