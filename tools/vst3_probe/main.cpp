// dc-vst3-probe: V7 verification tool. Loads VST3 plugins with JUCE the way mixd will, and reports
// whether each one can be instantiated, processed, edited through its parameter list and shown on screen.
//
//   dc-vst3-probe list  [--dir D]...                       list .vst3 bundles (nothing is loaded)
//   dc-vst3-probe probe <plugin.vst3> [options]            load in this process and test (see below)
//   dc-vst3-probe scan  [--dir D]... [options]             probe every bundle in its own child process
//   dc-vst3-probe gui   <plugin.vst3> [--type N]           open the editor and print parameter changes
//
// Options for probe / scan:
//   --json FILE         write the full report as JSON
//   --md FILE           (scan) write a Markdown summary table
//   --editor-seconds S  keep the editor open for S seconds while audio runs (default 2, 0 = skip editor)
//   --timeout S         (scan) kill a child that takes longer than S seconds (default 60)
//   --max-types N       probe at most N plugin types per bundle (shell plugins; default 4)
//   --type N            probe only type N of the bundle
//   --quiet             (probe) print nothing; used by scan for its children
//
// With no --dir, the standard VST3 folders are used (%LOCALAPPDATA%\Programs\Common\VST3,
// %CommonProgramFiles%\VST3, and VST3_PATH).
//
// Exit codes: 0 every probed plugin passed, 1 at least one failed, 2 usage error.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // JUCE's headers do not include <windows.h>, so its min/max macros would land here
#endif
#include <windows.h>
#include <psapi.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace juce;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSizes[] = {128, 256};
constexpr int kMaxParamsInJson = 4096;

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double workingSetMiB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    return 0.0;
#else
    long pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return static_cast<double>(resident) * 4096.0 / (1024.0 * 1024.0);
#endif
}

// A crashing plugin must end the child process at once, not leave a "has stopped working" box open.
void disableCrashDialogs() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

struct Args {
    String command;
    String target;
    StringArray dirs;
    File json, md;
    double editorSeconds = 2.0;
    double timeoutSeconds = 60.0;
    int maxTypes = 4;
    int onlyType = -1;
    bool quiet = false;
};

bool parseArgs(const StringArray& a, Args& out) {
    if (a.isEmpty()) return false;
    out.command = a[0];
    for (int i = 1; i < a.size(); ++i) {
        const auto& s = a[i];
        auto next = [&]() -> String { return i + 1 < a.size() ? a[++i] : String(); };
        if (s == "--dir") out.dirs.add(next());
        else if (s == "--json") out.json = File::getCurrentWorkingDirectory().getChildFile(next());
        else if (s == "--md") out.md = File::getCurrentWorkingDirectory().getChildFile(next());
        else if (s == "--editor-seconds") out.editorSeconds = next().getDoubleValue();
        else if (s == "--timeout") out.timeoutSeconds = next().getDoubleValue();
        else if (s == "--max-types") out.maxTypes = next().getIntValue();
        else if (s == "--type") out.onlyType = next().getIntValue();
        else if (s == "--quiet") out.quiet = true;
        else if (s.startsWith("--")) return false;
        else if (out.target.isEmpty()) out.target = s;
        else return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Parameter listener: separates changes the plugin makes by itself (its editor, its own automation)
// from the ones this host makes, which is what mixd needs to broadcast diffs to UIs and remotes.

class ParamWatcher final : public AudioProcessorParameter::Listener, public AudioProcessorListener {
public:
    void parameterValueChanged(int, float) override {
        (hostEditing_ ? hostEchoes_ : pluginChanges_).fetch_add(1);
    }
    void parameterGestureChanged(int, bool starting) override {
        if (starting) gestures_.fetch_add(1);
    }
    void audioProcessorParameterChanged(AudioProcessor*, int, float) override {}
    void audioProcessorChanged(AudioProcessor*, const ChangeDetails& d) override {
        if (d.parameterInfoChanged) paramInfoChanges_.fetch_add(1);
        if (d.latencyChanged) latencyChanges_.fetch_add(1);
    }

    std::atomic<bool> hostEditing_{false};
    std::atomic<int> hostEchoes_{0}, pluginChanges_{0}, gestures_{0}, paramInfoChanges_{0}, latencyChanges_{0};
};

struct HostEdit {
    explicit HostEdit(ParamWatcher& w) : w_(w) { w_.hostEditing_ = true; }
    ~HostEdit() { w_.hostEditing_ = false; }
    ParamWatcher& w_;
};

// ---------------------------------------------------------------------------------------------
// Audio: a test signal through the plugin, timed per block against the real-time budget.

struct BlockStats {
    int blocks = 0;
    double avgUs = 0.0, maxUs = 0.0, budgetUs = 0.0;
    bool finite = true;
    float peakOut = 0.0f;

    var toVar() const {
        auto o = std::make_unique<DynamicObject>();
        o->setProperty("blocks", blocks);
        o->setProperty("avg_us", avgUs);
        o->setProperty("max_us", maxUs);
        o->setProperty("budget_us", budgetUs);
        o->setProperty("avg_load_pct", budgetUs > 0 ? 100.0 * avgUs / budgetUs : 0.0);
        o->setProperty("max_load_pct", budgetUs > 0 ? 100.0 * maxUs / budgetUs : 0.0);
        o->setProperty("output_finite", finite);
        o->setProperty("peak_out", peakOut);
        return var(o.release());
    }
};

class AudioRunner {
public:
    AudioRunner(AudioPluginInstance& p, int blockSize) : plugin_(p), blockSize_(blockSize) {
        const int ch = std::max({p.getTotalNumInputChannels(), p.getTotalNumOutputChannels(), 1});
        buffer_.setSize(ch, blockSize);
    }

    void runBlock() {
        const int inCh = plugin_.getTotalNumInputChannels();
        for (int c = 0; c < buffer_.getNumChannels(); ++c) {
            auto* d = buffer_.getWritePointer(c);
            for (int i = 0; i < blockSize_; ++i)
                d[i] = c < inCh ? 0.25f * static_cast<float>(std::sin(phase_ + 0.0577 * i)) : 0.0f;
        }
        phase_ = std::fmod(phase_ + 0.0577 * blockSize_, 2.0 * MathConstants<double>::pi);

        midi_.clear();
        if (plugin_.acceptsMidi()) {
            // Instruments: hold a note for half a second, then release it.
            const int period = static_cast<int>(kSampleRate / blockSize_);
            if (stats_.blocks % period == 0) midi_.addEvent(MidiMessage::noteOn(1, 60, 0.8f), 0);
            if (stats_.blocks % period == period / 2) midi_.addEvent(MidiMessage::noteOff(1, 60), 0);
        }

        const auto t0 = Clock::now();
        plugin_.processBlock(buffer_, midi_);
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();

        for (int c = 0; c < plugin_.getTotalNumOutputChannels() && c < buffer_.getNumChannels(); ++c) {
            const auto* d = buffer_.getReadPointer(c);
            for (int i = 0; i < blockSize_; ++i) {
                if (!std::isfinite(d[i])) stats_.finite = false;
                else stats_.peakOut = std::max(stats_.peakOut, std::abs(d[i]));
            }
        }
        sumUs_ += us;
        stats_.maxUs = std::max(stats_.maxUs, us);
        ++stats_.blocks;
    }

    BlockStats finish() {
        stats_.avgUs = stats_.blocks ? sumUs_ / stats_.blocks : 0.0;
        stats_.budgetUs = 1e6 * blockSize_ / kSampleRate;
        return stats_;
    }

private:
    AudioPluginInstance& plugin_;
    int blockSize_;
    AudioBuffer<float> buffer_;
    MidiBuffer midi_;
    double phase_ = 0.0, sumUs_ = 0.0;
    BlockStats stats_;
};

// Runs the plugin on its own thread at real-time pace, the way mixd's audio thread will while the
// editor is open on the message thread.
class RealtimeAudioThread {
public:
    RealtimeAudioThread(AudioPluginInstance& p, int blockSize) : runner_(p, blockSize), blockSize_(blockSize) {
        thread_ = std::thread([this] {
            const auto period = std::chrono::duration<double>(blockSize_ / kSampleRate);
            auto next = Clock::now();
            while (!stop_) {
                runner_.runBlock();
                next += std::chrono::duration_cast<Clock::duration>(period);
                std::this_thread::sleep_until(next);
            }
        });
    }
    BlockStats stop() {
        stop_ = true;
        thread_.join();
        return runner_.finish();
    }

private:
    AudioRunner runner_;
    int blockSize_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// ---------------------------------------------------------------------------------------------

class EditorWindow final : public DocumentWindow {
public:
    explicit EditorWindow(const String& title)
        : DocumentWindow(title, Colours::darkgrey, DocumentWindow::closeButton) {}
    void closeButtonPressed() override { closed = true; }
    bool closed = false;
};

var describeBuses(AudioProcessor& p, bool input) {
    Array<var> buses;
    for (int i = 0; i < p.getBusCount(input); ++i) {
        auto* bus = p.getBus(input, i);
        auto o = std::make_unique<DynamicObject>();
        o->setProperty("name", bus->getName());
        o->setProperty("channels", bus->getNumberOfChannels());
        o->setProperty("layout", bus->getCurrentLayout().getDescription());
        o->setProperty("enabled", bus->isEnabled());
        buses.add(var(o.release()));
    }
    return buses;
}

bool trySetMainLayout(AudioProcessor& p, const AudioChannelSet& set) {
    auto layout = p.getBusesLayout();
    if (layout.inputBuses.size() > 0) layout.inputBuses.getReference(0) = set;
    if (layout.outputBuses.size() > 0) layout.outputBuses.getReference(0) = set;
    return p.checkBusesLayoutSupported(layout) && p.setBusesLayout(layout);
}

var describeParameter(AudioPluginInstance& p, int index) {
    auto* prm = p.getParameters()[index];
    auto o = std::make_unique<DynamicObject>();
    o->setProperty("index", index);
    if (auto* hosted = dynamic_cast<HostedAudioProcessorParameter*>(prm))
        o->setProperty("id", hosted->getParameterID());
    o->setProperty("name", prm->getName(64));
    o->setProperty("label", prm->getLabel());
    o->setProperty("value", prm->getValue());
    o->setProperty("default", prm->getDefaultValue());
    o->setProperty("text", prm->getCurrentValueAsText());
    o->setProperty("steps", prm->getNumSteps());
    o->setProperty("discrete", prm->isDiscrete());
    o->setProperty("boolean", prm->isBoolean());
    o->setProperty("automatable", prm->isAutomatable());
    o->setProperty("meta", prm->isMetaParameter());
    o->setProperty("category", static_cast<int>(prm->getCategory()));
    if (prm->isDiscrete() && prm->getNumSteps() > 1 && prm->getNumSteps() <= 16) {
        Array<var> names;
        for (int s = 0; s < prm->getNumSteps(); ++s)
            names.add(prm->getText(static_cast<float>(s) / static_cast<float>(prm->getNumSteps() - 1), 64));
        o->setProperty("value_texts", names);
    }
    return var(o.release());
}

// The parameter remote sliders would drive: first automatable, non-meta parameter that is not bypass.
AudioProcessorParameter* pickTestParameter(AudioPluginInstance& p) {
    auto* bypass = p.getBypassParameter();
    for (auto* prm : p.getParameters())
        if (prm->isAutomatable() && !prm->isMetaParameter() && prm != bypass) return prm;
    return p.getParameters().isEmpty() ? nullptr : p.getParameters()[0];
}

struct TypeResult {
    var json;
    bool ok = false;
    String summary;
};

TypeResult probeType(AudioPluginFormatManager& fm, const PluginDescription& desc, const Args& args) {
    auto r = std::make_unique<DynamicObject>();
    Array<var> problems;
    auto problem = [&](const String& s) { problems.add(s); };

    r->setProperty("name", desc.name);
    r->setProperty("vendor", desc.manufacturerName);
    r->setProperty("version", desc.version);
    r->setProperty("category", desc.category);
    r->setProperty("instrument", desc.isInstrument);
    r->setProperty("uid", String::toHexString(desc.uniqueId));

    const double memBefore = workingSetMiB();
    String error;
    auto t0 = Clock::now();
    auto plugin = fm.createPluginInstance(desc, kSampleRate, kBlockSizes[1], error);
    r->setProperty("create_ms", msSince(t0));
    if (!plugin) {
        r->setProperty("error", error);
        problem("create failed: " + error);
        r->setProperty("problems", problems);
        return {var(r.release()), false, "create failed: " + error};
    }
    r->setProperty("mem_after_create_mib", workingSetMiB() - memBefore);

    ParamWatcher watcher;
    plugin->addListener(&watcher);
    for (auto* prm : plugin->getParameters()) prm->addListener(&watcher);

    // Buses: mixd tracks are mono or stereo, so check both.
    r->setProperty("inputs", describeBuses(*plugin, true));
    r->setProperty("outputs", describeBuses(*plugin, false));
    const bool mono = trySetMainLayout(*plugin, AudioChannelSet::mono());
    const bool stereo = trySetMainLayout(*plugin, AudioChannelSet::stereo());
    r->setProperty("supports_mono", mono);
    r->setProperty("supports_stereo", stereo);
    if (!stereo) problem("stereo main bus not supported");
    r->setProperty("accepts_midi", plugin->acceptsMidi());
    r->setProperty("produces_midi", plugin->producesMidi());

    // Processing at the block sizes G2 targets (128-256), each after a fresh prepareToPlay.
    auto blocks = std::make_unique<DynamicObject>();
    for (int bs : kBlockSizes) {
        plugin->setRateAndBufferSizeDetails(kSampleRate, bs);
        plugin->prepareToPlay(kSampleRate, bs);
        AudioRunner runner(*plugin, bs);
        const int n = static_cast<int>(kSampleRate / bs); // one second of audio
        for (int i = 0; i < n; ++i) runner.runBlock();
        auto st = runner.finish();
        if (!st.finite) problem("NaN/Inf in output at block " + String(bs));
        if (st.avgUs > st.budgetUs * 0.5) problem("average block > 50% of budget at " + String(bs));
        blocks->setProperty(String(bs), st.toVar());
        plugin->releaseResources();
    }
    r->setProperty("process", var(blocks.release()));
    r->setProperty("latency_samples", plugin->getLatencySamples());
    r->setProperty("tail_seconds", plugin->getTailLengthSeconds());
    plugin->prepareToPlay(kSampleRate, kBlockSizes[1]);

    // Parameter list: what remotes show as sliders (decision #8).
    const auto& params = plugin->getParameters();
    r->setProperty("num_params", params.size());
    r->setProperty("has_bypass_param", plugin->getBypassParameter() != nullptr);
    Array<var> plist;
    for (int i = 0; i < params.size() && i < kMaxParamsInJson; ++i) plist.add(describeParameter(*plugin, i));
    r->setProperty("params", plist);

    // Host-side edit, read-back, text round trip.
    if (auto* prm = pickTestParameter(*plugin)) {
        auto o = std::make_unique<DynamicObject>();
        const float original = prm->getValue();
        const float wanted = prm->isDiscrete() || prm->isBoolean() ? (original < 0.5f ? 1.0f : 0.0f) : 0.25f;
        {
            HostEdit he(watcher);
            prm->beginChangeGesture();
            prm->setValueNotifyingHost(wanted);
            prm->endChangeGesture();
        }
        const float got = prm->getValue();
        const auto text = prm->getCurrentValueAsText();
        const float fromText = prm->getValueForText(text);
        o->setProperty("index", prm->getParameterIndex());
        o->setProperty("name", prm->getName(64));
        o->setProperty("set", wanted);
        o->setProperty("read_back", got);
        o->setProperty("text", text);
        o->setProperty("value_for_text", fromText);
        const float tol = prm->getNumSteps() > 1 && prm->getNumSteps() < 0x7fffffff
                              ? 0.5f / static_cast<float>(prm->getNumSteps() - 1) + 1e-4f
                              : 0.02f;
        const bool setOk = std::abs(got - wanted) <= tol;
        const bool textOk = std::abs(fromText - got) <= std::max(tol, 0.02f);
        o->setProperty("set_ok", setOk);
        o->setProperty("text_roundtrip_ok", textOk);
        if (!setOk) problem("parameter read-back differs from the value set");
        if (!textOk) problem("getValueForText(getText(v)) != v (remote numeric input would need care)");

        // State: save, change, restore, compare.
        MemoryBlock state;
        plugin->getStateInformation(state);
        o->setProperty("state_bytes", static_cast<int>(state.getSize()));
        {
            HostEdit he(watcher);
            prm->setValueNotifyingHost(wanted < 0.5f ? 0.9f : 0.1f);
        }
        plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        // Some plugins apply state asynchronously on the message thread.
        MessageManager::getInstance()->runDispatchLoopUntil(50);
        const bool stateOk = std::abs(prm->getValue() - got) <= tol;
        o->setProperty("state_restore_ok", stateOk);
        if (!stateOk) problem("setStateInformation did not restore the parameter");
        {
            HostEdit he(watcher);
            prm->setValueNotifyingHost(original);
        }
        r->setProperty("param_test", var(o.release()));
    } else {
        r->setProperty("param_test", var());
    }

    // Editor: open it on the message thread while audio runs on another thread.
    const bool hasEditor = plugin->hasEditor();
    r->setProperty("has_editor", hasEditor);
    bool editorOk = !hasEditor || args.editorSeconds <= 0;
    if (hasEditor && args.editorSeconds > 0) {
        auto e = std::make_unique<DynamicObject>();
        const int pluginChangesBefore = watcher.pluginChanges_.load();
        const double memBeforeEditor = workingSetMiB();
        RealtimeAudioThread audio(*plugin, kBlockSizes[1]);

        t0 = Clock::now();
        auto* editor = plugin->createEditorAndMakeActive();
        e->setProperty("create_ms", msSince(t0));
        if (editor != nullptr) {
            {
                EditorWindow window(desc.name);
                window.setUsingNativeTitleBar(true);
                window.setContentNonOwned(editor, true);
                window.setResizable(editor->isResizable(), false);
                window.centreWithSize(window.getWidth(), window.getHeight());
                window.setVisible(true);
                window.toFront(true);
                const auto shown = Clock::now();
                while (!window.closed && msSince(shown) < args.editorSeconds * 1000.0)
                    MessageManager::getInstance()->runDispatchLoopUntil(20);

                e->setProperty("width", editor->getWidth());
                e->setProperty("height", editor->getHeight());
                e->setProperty("resizable", editor->isResizable());
                e->setProperty("scale_factor", Desktop::getInstance().getDisplays().getPrimaryDisplay()
                                                   ? Desktop::getInstance().getDisplays().getPrimaryDisplay()->scale
                                                   : 1.0);
                e->setProperty("mem_mib", workingSetMiB() - memBeforeEditor);
                editorOk = editor->getWidth() > 0 && editor->getHeight() > 0;
                if (!editorOk) problem("editor has zero size");
                window.clearContentComponent();
            }
            t0 = Clock::now();
            plugin->editorBeingDeleted(editor);
            delete editor;
            MessageManager::getInstance()->runDispatchLoopUntil(20);
            e->setProperty("close_ms", msSince(t0));
            e->setProperty("closed_cleanly", plugin->getActiveEditor() == nullptr);
        } else {
            problem("hasEditor() but createEditorAndMakeActive() returned null");
            editorOk = false;
        }
        e->setProperty("audio_while_open", audio.stop().toVar());
        e->setProperty("plugin_param_changes", watcher.pluginChanges_.load() - pluginChangesBefore);
        r->setProperty("editor", var(e.release()));
    }
    r->setProperty("listener",
                   [&] {
                       auto o = std::make_unique<DynamicObject>();
                       o->setProperty("host_echoes", watcher.hostEchoes_.load());
                       o->setProperty("plugin_changes", watcher.pluginChanges_.load());
                       o->setProperty("gestures", watcher.gestures_.load());
                       o->setProperty("param_info_changes", watcher.paramInfoChanges_.load());
                       o->setProperty("latency_changes", watcher.latencyChanges_.load());
                       return var(o.release());
                   }());

    for (auto* prm : plugin->getParameters()) prm->removeListener(&watcher);
    plugin->removeListener(&watcher);
    plugin->releaseResources();
    t0 = Clock::now();
    plugin.reset();
    r->setProperty("destroy_ms", msSince(t0));

    const bool ok = problems.isEmpty() || (editorOk && !problems.isEmpty() && [&] {
        // Only the text round trip is a soft warning; everything else fails the plugin.
        for (auto& p : problems)
            if (!p.toString().startsWith("getValueForText")) return false;
        return true;
    }());
    r->setProperty("problems", problems);
    r->setProperty("ok", ok);

    String summary;
    summary << params.size() << " params";
    return {var(r.release()), ok, summary};
}

FileSearchPath searchPath(const Args& args, VST3PluginFormat& fmt) {
    if (args.dirs.isEmpty()) return fmt.getDefaultLocationsToSearch();
    FileSearchPath p;
    for (auto& d : args.dirs) p.add(File::getCurrentWorkingDirectory().getChildFile(d));
    return p;
}

StringArray findBundles(const Args& args) {
    VST3PluginFormat fmt;
    auto files = fmt.searchPathsForPlugins(searchPath(args, fmt), true, false);
    files.sort(true);
    return files;
}

void writeJson(const File& f, const var& v) {
    if (f == File()) return;
    f.getParentDirectory().createDirectory();
    f.replaceWithText(JSON::toString(v, false));
}

// probe: everything in this process. Returns the bundle report.
var probeBundle(const String& path, const Args& args, bool& allOk) {
    auto bundle = std::make_unique<DynamicObject>();
    bundle->setProperty("file", path);
    AudioPluginFormatManager fm;
    fm.addFormat(std::make_unique<VST3PluginFormat>());
    auto* fmt = fm.getFormat(0);

    OwnedArray<PluginDescription> types;
    auto t0 = Clock::now();
    fmt->findAllTypesForFile(types, path);
    bundle->setProperty("scan_ms", msSince(t0));
    bundle->setProperty("num_types", types.size());

    Array<var> results;
    allOk = !types.isEmpty();
    if (types.isEmpty()) bundle->setProperty("error", "no plugin types found in the bundle");
    for (int i = 0; i < types.size(); ++i) {
        if (args.onlyType >= 0 ? i != args.onlyType : i >= args.maxTypes) continue;
        auto res = probeType(fm, *types[i], args);
        allOk = allOk && res.ok;
        results.add(res.json);
        if (!args.quiet) {
            const auto& j = res.json;
            std::cout << (res.ok ? "  OK   " : "  FAIL ") << j["name"].toString() << " (" << j["vendor"].toString()
                      << ")  params=" << j["num_params"].toString()
                      << "  editor=" << (j["editor"].isObject() ? j["editor"]["width"].toString() + "x" +
                                                               j["editor"]["height"].toString()
                                                         : String(j["has_editor"] ? "not opened" : "none"))
                      << "\n";
            for (auto& p : *j["problems"].getArray()) std::cout << "         - " << p.toString() << "\n";
        }
    }
    bundle->setProperty("types", results);
    bundle->setProperty("ok", allOk);
    return var(bundle.release());
}

int cmdList(const Args& args) {
    VST3PluginFormat fmt;
    std::cout << "search path: " << searchPath(args, fmt).toString() << "\n";
    for (auto& f : findBundles(args)) std::cout << f << "\n";
    return 0;
}

int cmdProbe(const Args& args) {
    if (args.target.isEmpty()) return 2;
    if (args.quiet) disableCrashDialogs();
    bool ok = false;
    if (!args.quiet) std::cout << args.target << "\n";
    auto report = probeBundle(File::getCurrentWorkingDirectory().getChildFile(args.target).getFullPathName(), args, ok);
    writeJson(args.json, report);
    if (!args.quiet) std::cout << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}

String exitDescription(uint32 code) {
    switch (code) {
    case 0xC0000005u: return "access violation";
    case 0xC00000FDu: return "stack overflow";
    case 0xC0000409u: return "stack buffer overrun / fail-fast";
    case 0xC0000374u: return "heap corruption";
    case 0x80000003u: return "breakpoint";
    default: return "exit code 0x" + String::toHexString(static_cast<int>(code));
    }
}

String cell(const var& v) { return v.toString().replace("|", "\\|"); }

String fmtMs(const var& v) { return v.isVoid() ? String("-") : String(static_cast<double>(v), 0); }

int cmdScan(const Args& args) {
    const auto bundles = findBundles(args);
    const auto self = File::getSpecialLocation(File::currentExecutableFile).getFullPathName();
    const auto tmp = File::getSpecialLocation(File::tempDirectory).getChildFile("dc-vst3-probe");
    tmp.createDirectory();

    std::cout << bundles.size() << " bundle(s)\n";
    Array<var> all;
    int passed = 0, failed = 0, crashed = 0;
    for (int b = 0; b < bundles.size(); ++b) {
        const auto& path = bundles[b];
        std::cout << "[" << (b + 1) << "/" << bundles.size() << "] " << path << "\n" << std::flush;
        const auto jsonFile = tmp.getChildFile("probe-" + String(b) + ".json");
        jsonFile.deleteFile();

        StringArray cmd{self, "probe", path, "--quiet", "--json", jsonFile.getFullPathName(),
                        "--editor-seconds", String(args.editorSeconds), "--max-types", String(args.maxTypes)};
        ChildProcess child;
        const auto t0 = Clock::now();
        var report;
        if (!child.start(cmd, 0)) {
            auto o = std::make_unique<DynamicObject>();
            o->setProperty("file", path);
            o->setProperty("error", "could not start child process");
            report = var(o.release());
        } else {
            const bool finished = child.waitForProcessToFinish(static_cast<int>(args.timeoutSeconds * 1000));
            if (!finished) child.kill();
            const auto code = finished ? child.getExitCode() : 0u;
            report = JSON::parse(jsonFile);
            if (!report.isObject()) {
                auto o = std::make_unique<DynamicObject>();
                o->setProperty("file", path);
                o->setProperty("ok", false);
                o->setProperty("crashed", finished);
                o->setProperty("hung", !finished);
                o->setProperty("error", finished ? "child died: " + exitDescription(code)
                                                 : "timed out after " + String(args.timeoutSeconds) + " s (killed)");
                report = var(o.release());
            }
            report.getDynamicObject()->setProperty("child_ms", msSince(t0));
        }
        const bool ok = report["ok"];
        if (ok) ++passed;
        else if (report["crashed"] || report["hung"]) ++crashed;
        else ++failed;
        std::cout << "       " << (ok ? "OK" : report["crashed"] ? "CRASH" : report["hung"] ? "HUNG" : "FAIL");
        if (report.hasProperty("error")) std::cout << "  " << report["error"].toString();
        if (auto* types = report["types"].getArray())
            for (auto& t : *types) {
                std::cout << "\n       - " << t["name"].toString() << ": " << t["num_params"].toString() << " params";
                if (t["has_editor"]) std::cout << ", editor " << t["editor"]["width"].toString() << "x"
                                               << t["editor"]["height"].toString();
                if (auto* pr = t["problems"].getArray())
                    for (auto& p : *pr) std::cout << "\n           ! " << p.toString();
            }
        std::cout << "\n" << std::flush;
        all.add(report);
    }

    auto root = std::make_unique<DynamicObject>();
    root->setProperty("tool", "dc-vst3-probe");
    root->setProperty("juce", SystemStats::getJUCEVersion());
    root->setProperty("os", SystemStats::getOperatingSystemName());
    root->setProperty("cpu", SystemStats::getCpuModel());
    root->setProperty("date", Time::getCurrentTime().toISO8601(true));
    root->setProperty("bundles", all);
    root->setProperty("passed", passed);
    root->setProperty("failed", failed);
    root->setProperty("crashed", crashed);
    var rootVar(root.release());
    writeJson(args.json, rootVar);

    if (args.md != File()) {
        String md;
        md << "| Plugin | Vendor | Category | Result | Load ms | Params | Editor | Avg/Max load @256 | Notes |\n"
           << "|---|---|---|---|---|---|---|---|---|\n";
        for (auto& b : all) {
            auto* types = b["types"].getArray();
            if (types == nullptr || types->isEmpty()) {
                md << "| " << File(b["file"].toString()).getFileNameWithoutExtension() << " | | | "
                   << (b["crashed"] ? "CRASH" : b["hung"] ? "HUNG" : "FAIL") << " | | | | | "
                   << cell(b["error"]) << " |\n";
                continue;
            }
            for (auto& t : *types) {
                const auto& p256 = t["process"]["256"];
                StringArray notes;
                if (auto* pr = t["problems"].getArray())
                    for (auto& p : *pr) notes.add(p.toString());
                md << "| " << cell(t["name"]) << " | " << cell(t["vendor"]) << " | " << cell(t["category"]) << " | " << (t["ok"] ? "OK" : "FAIL") << " | "
                   << fmtMs(t["create_ms"]) << " | " << t["num_params"].toString() << " | "
                   << (t["has_editor"] ? t["editor"]["width"].toString() + "x" + t["editor"]["height"].toString()
                                       : String("none"))
                   << " | " << String(static_cast<double>(p256["avg_load_pct"]), 2) << "% / "
                   << String(static_cast<double>(p256["max_load_pct"]), 2) << "% | " << cell(notes.joinIntoString("; "))
                   << " |\n";
            }
        }
        args.md.getParentDirectory().createDirectory();
        args.md.replaceWithText(md);
    }

    std::cout << "passed " << passed << ", failed " << failed << ", crashed/hung " << crashed << "\n";
    return failed + crashed == 0 ? 0 : 1;
}

// gui: interactive check that edits made in the plugin's own window reach the host.
int cmdGui(const Args& args) {
    if (args.target.isEmpty()) return 2;
    AudioPluginFormatManager fm;
    fm.addFormat(std::make_unique<VST3PluginFormat>());
    OwnedArray<PluginDescription> types;
    fm.getFormat(0)->findAllTypesForFile(types, File::getCurrentWorkingDirectory().getChildFile(args.target).getFullPathName());
    const int idx = std::max(0, args.onlyType);
    if (idx >= types.size()) {
        std::cerr << "no plugin type " << idx << " in " << args.target << "\n";
        return 1;
    }
    String error;
    auto plugin = fm.createPluginInstance(*types[idx], kSampleRate, kBlockSizes[1], error);
    if (!plugin) {
        std::cerr << "create failed: " << error << "\n";
        return 1;
    }
    struct Printer final : AudioProcessorParameter::Listener {
        explicit Printer(AudioPluginInstance& p) : plugin(p) {}
        void parameterValueChanged(int i, float v) override {
            // May arrive on any thread; printing is fine for an interactive tool.
            auto* prm = plugin.getParameters()[i];
            std::cout << "param " << i << " " << prm->getName(32) << " = " << v << " (" << prm->getText(v, 32)
                      << ")\n" << std::flush;
        }
        void parameterGestureChanged(int i, bool start) override {
            std::cout << "gesture " << i << (start ? " begin" : " end") << "\n" << std::flush;
        }
        AudioPluginInstance& plugin;
    } printer(*plugin);
    for (auto* prm : plugin->getParameters()) prm->addListener(&printer);

    plugin->prepareToPlay(kSampleRate, kBlockSizes[1]);
    RealtimeAudioThread audio(*plugin, kBlockSizes[1]);
    auto* editor = plugin->createEditorAndMakeActive();
    if (editor == nullptr) {
        std::cerr << "plugin has no editor\n";
        audio.stop();
        return 1;
    }
    std::cout << "editor open; move controls in the plugin window, close it to finish\n";
    {
        EditorWindow window(types[idx]->name);
        window.setUsingNativeTitleBar(true);
        window.setContentNonOwned(editor, true);
        window.setResizable(editor->isResizable(), false);
        window.centreWithSize(window.getWidth(), window.getHeight());
        window.setVisible(true);
        while (!window.closed) MessageManager::getInstance()->runDispatchLoopUntil(50);
        window.clearContentComponent();
    }
    plugin->editorBeingDeleted(editor);
    delete editor;
    auto st = audio.stop();
    std::cout << "audio blocks while open: " << st.blocks << ", max " << st.maxUs << " us\n";
    for (auto* prm : plugin->getParameters()) prm->removeListener(&printer);
    return 0;
}

void usage() {
    std::cerr << "usage: dc-vst3-probe list|probe|scan|gui ... (see the header of tools/vst3_probe/main.cpp)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    StringArray raw;
    for (int i = 1; i < argc; ++i) raw.add(String::fromUTF8(argv[i]));
    Args args;
    if (!parseArgs(raw, args)) {
        usage();
        return 2;
    }

    ScopedJuceInitialiser_GUI juce;
    int rc = 2;
    if (args.command == "list") rc = cmdList(args);
    else if (args.command == "probe") rc = cmdProbe(args);
    else if (args.command == "scan") rc = cmdScan(args);
    else if (args.command == "gui") rc = cmdGui(args);
    if (rc == 2) usage();
    return rc;
}
