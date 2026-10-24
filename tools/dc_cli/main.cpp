// dc-cli: command line client for the control protocol (works against mixd or dc-mock-server).
//
//   dc-cli [--host H] [--port P] snapshot [PREFIX]   print the parameter tree (optionally a subtree)
//   dc-cli [--host H] [--port P] set PATH VALUE       change one parameter
//   dc-cli [--host H] [--port P] call METHOD [k=v...] invoke a command
//   dc-cli [--host H] [--port P] watch [--meters]     print deltas and events until Ctrl+C
//   dc-cli schema [--markdown]                         print the protocol v1 schema (offline)

#include "dc/control/client.hpp"
#include "dc/protocol/meter.hpp"
#include "dc/protocol/schema.hpp"
#include "dc/version.hpp"

#include <asio.hpp>

#include <array>
#include <charconv>
#include <cstdio>
#include <functional>
#include <optional>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace dc::proto;

namespace {

int usage() {
    std::cerr << "usage:\n"
                 "  dc-cli [--host H] [--port P] snapshot [PREFIX]\n"
                 "  dc-cli [--host H] [--port P] set PATH VALUE\n"
                 "  dc-cli [--host H] [--port P] call METHOD [key=value ...]\n"
                 "  dc-cli [--host H] [--port P] watch [--meters]\n"
                 "  dc-cli schema [--markdown]\n";
    return 2;
}

// Parses a command line value. With a spec the declared type wins; otherwise the text is guessed.
Value parseValue(const std::string& text, const ParamSpec* spec) {
    auto asInt = [&]() -> std::optional<std::int64_t> {
        std::int64_t v = 0;
        auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
        if (ec == std::errc() && p == text.data() + text.size()) return v;
        return std::nullopt;
    };
    auto asDouble = [&]() -> std::optional<double> {
        try {
            std::size_t used = 0;
            double d = std::stod(text, &used);
            if (used == text.size()) return d;
        } catch (...) {
        }
        return std::nullopt;
    };
    if (spec) {
        switch (spec->type) {
        case ValueType::Bool: return text == "true" || text == "1" || text == "on";
        case ValueType::Int:
            if (auto i = asInt()) return *i;
            break;
        case ValueType::Float:
            if (auto d = asDouble()) return *d;
            break;
        case ValueType::String: return text;
        case ValueType::Nil: return std::monostate{};
        }
        return text; // let the server report the type mismatch
    }
    if (text == "true") return true;
    if (text == "false") return false;
    if (text == "nil") return std::monostate{};
    if (auto i = asInt()) return *i;
    if (auto d = asDouble()) return *d;
    return text;
}

void printResult(const Result& r) {
    if (r.code == ErrorCode::Ok) {
        std::cout << "ok";
        if (!std::holds_alternative<std::monostate>(r.value)) std::cout << " " << toDisplayString(r.value);
        std::cout << "\n";
    } else {
        std::cout << "error " << errorName(r.code) << ": " << r.message;
        if (!r.path.empty()) std::cout << " (" << r.path << ")";
        std::cout << "\n";
    }
}

std::string rangeText(const ParamSpec& s) {
    std::string out;
    if (!s.choices.empty()) {
        for (const auto& c : s.choices) out += (out.empty() ? "" : " / ") + c;
    } else if (!s.intChoices.empty()) {
        for (auto c : s.intChoices) out += (out.empty() ? "" : " / ") + std::to_string(c);
    } else if (s.min || s.max) {
        auto num = [&](double d) {
            char buf[32];
            if (s.type == ValueType::Int && d > -1e15 && d < 1e15) std::snprintf(buf, sizeof buf, "%.0f", d);
            else std::snprintf(buf, sizeof buf, "%g", d);
            return std::string(buf);
        };
        out = (s.min ? num(*s.min) : "") + " 〜 " + (s.max ? num(*s.max) : "");
    }
    return out;
}

int printSchema(bool markdown) {
    const auto& schema = schemaV1();
    if (markdown) {
        std::cout << "| パス | 型 | 書込 | 範囲・選択肢 | 既定値 | 説明 |\n|---|---|---|---|---|---|\n";
        for (const auto& s : schema.params()) {
            std::cout << "| `" << s.pattern << "` | " << typeName(s.type) << " | "
                      << (s.access == Access::ReadOnly ? "ro" : "rw") << " | " << rangeText(s) << " | `"
                      << toDisplayString(s.defaultValue) << "` | " << s.description << " |\n";
        }
        std::cout << "\n| メソッド | 引数 | 戻り値 | 実装フェーズ | 説明 |\n|---|---|---|---|---|\n";
        for (const auto& m : schema.methods()) {
            std::cout << "| `" << m.pattern << "` | " << (m.args.empty() ? "なし" : m.args) << " | " << m.returns
                      << " | " << m.since << " | " << m.description << " |\n";
        }
    } else {
        for (const auto& s : schema.params())
            std::cout << s.pattern << "  " << typeName(s.type) << (s.access == Access::ReadOnly ? " ro" : " rw")
                      << "  [" << rangeText(s) << "]  default " << toDisplayString(s.defaultValue) << "\n";
        for (const auto& m : schema.methods()) std::cout << "call " << m.pattern << " (" << m.args << ")\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    dc::control::ControlClient::Options opt;
    opt.clientName = "dc-cli";
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) opt.host = argv[++i];
        else if (a == "--port" && i + 1 < argc) opt.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        else if (a == "--version") {
            std::cout << "dc-cli " << dc::kVersionString << "\n";
            return 0;
        } else args.push_back(a);
    }
    if (args.empty()) return usage();
    const std::string cmd = args[0];

    if (cmd == "schema") return printSchema(args.size() > 1 && args[1] == "--markdown");

    const bool watchMeters = cmd == "watch" && args.size() > 1 && args[1] == "--meters";
    asio::io_context meterIo;
    std::optional<asio::ip::udp::socket> meterSocket;
    if (watchMeters) {
        meterSocket.emplace(meterIo, asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
        opt.meterPort = meterSocket->local_endpoint().port();
    }

    dc::control::ControlClient client(opt);
    if (cmd == "watch") {
        client.onDelta = [](const Delta& d) {
            std::cout << "delta rev " << d.rev;
            if (!d.originClient.empty()) std::cout << " by " << d.originClient;
            std::cout << "\n";
            for (const auto& r : d.removed) std::cout << "  - " << r << "\n";
            for (const auto& c : d.changes) std::cout << "  " << c.path << " = " << toDisplayString(c.value) << "\n";
        };
        client.onEvent = [](const Event& e) {
            std::cout << "event " << e.name;
            for (const auto& [k, v] : e.data) std::cout << " " << k << "=" << toDisplayString(v);
            std::cout << "\n";
        };
        client.onDisconnected = [](const std::string& why) { std::cout << "disconnected: " << why << "\n"; };
    }

    std::string err;
    if (!client.connect(std::chrono::seconds(5), &err)) {
        std::cerr << "cannot connect to " << opt.host << ":" << opt.port << ": " << err << "\n";
        return 1;
    }

    if (cmd == "snapshot") {
        const std::string prefix = args.size() > 1 ? args[1] : "";
        const auto tree = client.tree();
        std::cout << "# rev " << tree.rev() << ", server " << client.welcome().serverName << "\n";
        for (const auto& [path, v] : tree.values())
            if (path.compare(0, prefix.size(), prefix) == 0) std::cout << path << " = " << toDisplayString(v) << "\n";
        return 0;
    }
    if (cmd == "set") {
        if (args.size() != 3) return usage();
        const Value v = parseValue(args[2], schemaV1().find(args[1]));
        const auto r = client.set({{args[1], v}}).get();
        printResult(r);
        return r.code == ErrorCode::Ok ? 0 : 1;
    }
    if (cmd == "call") {
        if (args.size() < 2) return usage();
        Args callArgs;
        for (std::size_t i = 2; i < args.size(); ++i) {
            const auto eq = args[i].find('=');
            if (eq == std::string::npos) return usage();
            callArgs[args[i].substr(0, eq)] = parseValue(args[i].substr(eq + 1), nullptr);
        }
        const auto r = client.call(args[1], callArgs).get();
        printResult(r);
        return r.code == ErrorCode::Ok ? 0 : 1;
    }
    if (cmd == "watch") {
        std::cout << "watching rev " << client.tree().rev() << " (Ctrl+C to stop)\n";
        asio::signal_set signals(meterIo, SIGINT, SIGTERM);
        signals.async_wait([&](const asio::error_code&, int) { meterIo.stop(); });
        std::array<std::uint8_t, 2048> buf{};
        asio::ip::udp::endpoint from;
        std::function<void()> receive = [&] {
            meterSocket->async_receive_from(asio::buffer(buf), from, [&](asio::error_code ec, std::size_t n) {
                if (ec) return;
                if (auto p = decodeMeterPacket({buf.data(), n}); p && p->seq % 30 == 0) {
                    std::cout << "meters seq " << p->seq << ":";
                    for (const auto& e : p->entries) std::cout << " " << e.meterId << "=" << e.peakCdb[0] / 100 << "dB";
                    std::cout << "\n";
                }
                receive();
            });
        };
        if (meterSocket) receive();
        meterIo.run();
        return 0;
    }
    return usage();
}
