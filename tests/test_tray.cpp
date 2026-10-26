// dc-tray (P5-B): restart policy, command-line quoting, the settings file, Windows' startup-approved value, log
// rotation and the status file.

#include "tray_core.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace dc::tray;

TEST_CASE("restart: waits longer after each quick failure, then gives up", "[tray]") {
    RestartTracker t;
    CHECK(t.onExit(500) == 1000);
    CHECK(t.onExit(500) == 3000);
    CHECK(t.onExit(500) == 10000);
    CHECK(t.onExit(500) == -1);
    CHECK(t.failures() == 4);
    t.reset();
    CHECK(t.onExit(0) == 1000);
}

TEST_CASE("restart: a long run starts the count again", "[tray]") {
    RestartTracker t;
    CHECK(t.onExit(1000) == 1000);
    CHECK(t.onExit(1000) == 3000);
    CHECK(t.onExit(60000) == 1000);  // crashed after a minute: first failure of a new series
    CHECK(t.failures() == 1);
    CHECK(t.onExit(1000) == 3000);
}

TEST_CASE("restart: a crash after hours is restarted every time", "[tray]") {
    RestartTracker t;
    for (int i = 0; i < 10; ++i) CHECK(t.onExit(3'600'000) == 1000);
}

TEST_CASE("restart: own policy", "[tray]") {
    RestartPolicy p;
    p.delaysMs = {50};
    p.maxFailures = 2;
    RestartTracker t(p);
    CHECK(t.onExit(0) == 50);
    CHECK(t.onExit(0) == 50);
    CHECK(t.onExit(0) == -1);
}

TEST_CASE("command line: arguments are quoted as Windows reads them back", "[tray]") {
    CHECK(quoteArg("plain") == "plain");
    CHECK(quoteArg("") == "\"\"");
    CHECK(quoteArg("with space") == "\"with space\"");
    CHECK(quoteArg(R"(C:\Program Files\DC\mixd.exe)") == R"("C:\Program Files\DC\mixd.exe")");
    CHECK(quoteArg(R"(C:\dir\)") == R"(C:\dir\)");                 // no quoting needed: backslashes stay
    CHECK(quoteArg(R"(C:\my dir\)") == R"("C:\my dir\\")");        // a trailing backslash before our quote doubles
    CHECK(quoteArg(R"(say "hi")") == R"("say \"hi\"")");
    CHECK(quoteArg(R"(a\"b)") == R"("a\\\"b")");
    CHECK(quoteArg("Local\\DesktopCapture.stop.mixd") == "Local\\DesktopCapture.stop.mixd");
}

TEST_CASE("command line: exe, arguments and the settings fragment", "[tray]") {
    CHECK(buildCommandLine(R"(C:\MY APP\mixd.exe)", {"--stop-event", "Local\\x"}, "  --no-asio --quiet") ==
          R"("C:\MY APP\mixd.exe" --stop-event Local\x --no-asio --quiet)");
    CHECK(buildCommandLine("capd.exe", {}, "") == "capd.exe");
    CHECK(buildCommandLine("capd.exe", {}, "   ") == "capd.exe");
}

TEST_CASE("settings file", "[tray]") {
    std::vector<std::string> errors;
    const TrayConfig c = parseConfig("\xEF\xBB\xBF# comment\r\n"
                                     "mixd_args = --no-asio --quiet\r\n"
                                     "\r\n"
                                     "  capd_args=--no-video  \r\n"
                                     "ui_args = --page video\n"
                                     "capd = off\n",
                                     &errors);
    CHECK(errors.empty());
    CHECK(c.mixdArgs == "--no-asio --quiet");
    CHECK(c.capdArgs == "--no-video");
    CHECK(c.uiArgs == "--page video");
    CHECK_FALSE(c.capd);

    const TrayConfig d = parseConfig("", &errors);
    CHECK(d.capd);
    CHECK(d.mixdArgs.empty());

    errors.clear();
    const TrayConfig e = parseConfig("colour = blue\ncapd = maybe\nnothing here\n", &errors);
    CHECK(e.capd);
    REQUIRE(errors.size() == 3);
    CHECK(errors[0] == "line 1: unknown key \"colour\"");
    CHECK(errors[1] == "line 2: capd must be on or off");
    CHECK(errors[2] == "line 3: no '=' in \"nothing here\"");
}

TEST_CASE("startup approved: Windows' on/off mark", "[tray]") {
    CHECK(startupApproved({}));
    CHECK(startupApproved({0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(startupApproved({0x06, 0, 0, 0}));
    CHECK_FALSE(startupApproved({0x03, 0, 0, 0, 0x5A, 0x1F, 0, 0, 0, 0, 0, 0}));
    CHECK_FALSE(startupApproved({0x07}));
}

TEST_CASE("logs: older runs move one place down, the oldest goes", "[tray]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dc_tray_test_logs";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto write = [&](const char* name, const char* text) { std::ofstream(dir / name) << text; };
    auto read = [&](const char* name) {
        std::ifstream in(dir / name);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    write("mixd.log", "run3");
    write("mixd.1.log", "run2");
    write("mixd.2.log", "run1");
    rotateLogs(dir, "mixd", 3);
    CHECK_FALSE(fs::exists(dir / "mixd.log"));
    CHECK(read("mixd.1.log") == "run3");
    CHECK(read("mixd.2.log") == "run2");
    CHECK_FALSE(fs::exists(dir / "mixd.3.log"));

    rotateLogs(dir, "capd", 3);  // nothing yet: no error
    CHECK_FALSE(fs::exists(dir / "capd.1.log"));
    fs::remove_all(dir);
}

TEST_CASE("exit codes in words", "[tray]") {
    CHECK(describeExit(0) == "終了コード 0");
    CHECK(describeExit(2) == "終了コード 2");
    CHECK(describeExit(0xC0000005u) == "異常終了（0xC0000005 アクセス違反）");
    CHECK(describeExit(0xC0000123u) == "異常終了（0xC0000123）");
}

TEST_CASE("status file and icon colour", "[tray]") {
    std::vector<ChildStatus> s(2);
    s[0].name = "mixd";
    s[0].state = ChildState::Running;
    s[0].pid = 42;
    s[1].name = "capd";
    s[1].state = ChildState::Restarting;
    s[1].restarts = 1;
    s[1].hasExit = true;
    s[1].lastExit = 0xC0000005u;
    CHECK(formatStatus(7, s) ==
          "tray_pid=7\n"
          "mixd.state=running\nmixd.pid=42\nmixd.restarts=0\n"
          "capd.state=restarting\ncapd.pid=0\ncapd.restarts=1\ncapd.last_exit=0xC0000005\n");
    CHECK(overallHealth(s) == Health::Busy);
    s[1].state = ChildState::Running;
    CHECK(overallHealth(s) == Health::Ok);
    s[1].state = ChildState::Failed;
    CHECK(overallHealth(s) == Health::Failed);
    s[0].state = s[1].state = ChildState::Stopped;
    CHECK(overallHealth(s) == Health::Stopped);
    CHECK(overallHealth({}) == Health::Stopped);
}
