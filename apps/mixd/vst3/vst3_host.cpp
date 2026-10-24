#include "vst3_host.hpp"

#include "dc/protocol/fx.hpp"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace dc::mixd::vst3 {

using namespace juce;

namespace {

constexpr double kRate = 48000.0;
constexpr int kMaxChannels = 32; // AudioBuffer refers to up to 32 channels without allocating

std::string str(const String& s) { return s.toStdString(); }

std::string idFor(const PluginDescription& d) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(d.uniqueId));
    return std::string(proto::kVst3TypePrefix) + buf;
}

// Kontakt's "#000" host-automation slots and parameters without a name carry no plugin state (V7).
bool isPlaceholder(AudioProcessorParameter* p) {
    const auto name = p->getName(64).trim();
    return name.isEmpty() || name.startsWithChar('#');
}

bool trySetMainLayout(AudioProcessor& p, const AudioChannelSet& set) {
    auto layout = p.getBusesLayout();
    if (layout.inputBuses.size() > 0) layout.inputBuses.getReference(0) = set;
    if (layout.outputBuses.size() > 0) layout.outputBuses.getReference(0) = set;
    return p.checkBusesLayoutSupported(layout) && p.setBusesLayout(layout);
}

// Puts the plugin on a stereo (or mono) main bus. Returns the main bus channel count, 0 if neither works.
int setupLayout(AudioPluginInstance& p) {
    if (trySetMainLayout(p, AudioChannelSet::stereo())) return 2;
    if (trySetMainLayout(p, AudioChannelSet::mono())) return 1;
    const auto out = p.getBusesLayout().getMainOutputChannelSet().size();
    return out == 1 || out == 2 ? out : 0; // multi-bus plugins that keep their own layout
}

// Runs f on the message thread and returns its result (directly when already there). Gives up after
// a few seconds so a busy plugin window cannot stall the engine's network thread for good.
template <typename T, typename F>
T onMessageThread(F f, T fallback) {
    auto* mm = MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr) return fallback;
    if (mm->isThisTheMessageThread()) return f();
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();
    if (!MessageManager::callAsync([promise, f]() mutable { promise->set_value(f()); })) return fallback;
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return fallback;
    return future.get();
}

void runOnMessageThread(std::function<void()> f) {
    auto* mm = MessageManager::getInstanceWithoutCreating();
    if (mm != nullptr && mm->isThisTheMessageThread()) f();
    else MessageManager::callAsync(std::move(f));
}

class EditorWindow final : public DocumentWindow {
public:
    EditorWindow(const String& title, std::function<void()> onClose)
        : DocumentWindow(title, Colours::darkgrey, DocumentWindow::closeButton), onClose_(std::move(onClose)) {}
    void closeButtonPressed() override { onClose_(); }

private:
    std::function<void()> onClose_;
};

// ---------------------------------------------------------------------------------------------

class Instance final : public PluginInstance,
                       public std::enable_shared_from_this<Instance>,
                       private AudioProcessorParameter::Listener {
public:
    // Message thread.
    Instance(std::unique_ptr<AudioPluginInstance> plugin, int mainChannels)
        : plugin_(std::move(plugin)), main_(mainChannels) {
        name_ = str(plugin_->getName());
        hasEditor_ = plugin_->hasEditor(); // asks the plugin for a view: message thread only
        auto* bypass = plugin_->getBypassParameter();
        const auto& all = plugin_->getParameters();
        for (int i = 0; i < all.size(); ++i) {
            auto* p = all[i];
            if (p == bypass || isPlaceholder(p)) continue;
            auto* hosted = dynamic_cast<HostedAudioProcessorParameter*>(p);
            if (hosted == nullptr) continue;
            const auto id = static_cast<std::uint32_t>(hosted->getParameterID().getLargeIntValue());
            Param e{p, proto::vst3ParamKey(id)};
            byKey_[e.key] = params_.size();
            byIndex_[p->getParameterIndex()] = params_.size();
            params_.push_back(std::move(e));
            p->addListener(this);
        }
        total_ = std::clamp(std::max({plugin_->getTotalNumInputChannels(), plugin_->getTotalNumOutputChannels(), main_}), 1,
                            kMaxChannels);
        for (int c = 0; c < total_; ++c) {
            scratch_[static_cast<std::size_t>(c)].assign(kFxMaxFrames, 0.0f);
            ptrs_[static_cast<std::size_t>(c)] = scratch_[static_cast<std::size_t>(c)].data();
        }
        midi_.ensureSize(256);
        plugin_->setRateAndBufferSizeDetails(kRate, kFxMaxFrames);
        plugin_->prepareToPlay(kRate, kFxMaxFrames);
    }

    ~Instance() override {
        for (auto& p : params_) p.p->removeListener(this);
        // the window and the plugin must go away on the message thread, the window first
        auto window = std::make_shared<std::unique_ptr<EditorWindow>>(std::move(window_));
        auto plugin = std::make_shared<std::unique_ptr<AudioPluginInstance>>(std::move(plugin_));
        runOnMessageThread([window, plugin] {
            window->reset();
            if (*plugin) (*plugin)->releaseResources();
            plugin->reset();
        });
    }

    std::string name() const override { return name_; }

    void setListener(PluginInstance::Listener l) override {
        std::lock_guard lock(listenerMutex_);
        listener_ = std::move(l);
    }

    int paramCount() const override { return static_cast<int>(params_.size()); }

    std::vector<PluginParamInfo> params(int offset, int count) const override {
        std::vector<PluginParamInfo> out;
        for (int i = std::max(0, offset); i < offset + count && i < paramCount(); ++i) out.push_back(info(params_[static_cast<std::size_t>(i)]));
        return out;
    }

    std::optional<PluginParamInfo> param(const std::string& key) const override {
        auto it = byKey_.find(key);
        if (it == byKey_.end()) return std::nullopt;
        return info(params_[it->second]);
    }

    void setParam(std::string_view key, double value) override {
        auto it = byKey_.find(std::string(key));
        if (it == byKey_.end()) return;
        auto* p = params_[it->second].p;
        const auto v = static_cast<float>(std::clamp(value, 0.0, 1.0));
        // the tree echoes what the plugin reported; setting it again would only make noise
        if (std::abs(p->getValue() - v) < 1e-6f) return;
        p->setValue(v); // lock-free for hosted VST3: reaches the processor and the plugin's controller
    }

    void process(float* const* x, int channels, int frames) override {
        frames = std::min(frames, kFxMaxFrames);
        const auto bytes = static_cast<std::size_t>(frames) * sizeof(float);
        float* s0 = ptrs_[0];
        if (main_ == 2) {
            std::memcpy(s0, x[0], bytes);
            std::memcpy(ptrs_[1], x[channels > 1 ? 1 : 0], bytes);
        } else if (channels > 1) {
            for (int i = 0; i < frames; ++i) s0[i] = (x[0][i] + x[1][i]) * 0.5f;
        } else {
            std::memcpy(s0, x[0], bytes);
        }
        for (int c = main_; c < total_; ++c) std::memset(ptrs_[static_cast<std::size_t>(c)], 0, bytes);

        AudioBuffer<float> buffer(ptrs_.data(), total_, frames);
        midi_.clear();
        plugin_->processBlock(buffer, midi_);

        if (main_ == 2) {
            if (channels > 1) {
                std::memcpy(x[0], s0, bytes);
                std::memcpy(x[1], ptrs_[1], bytes);
            } else {
                for (int i = 0; i < frames; ++i) x[0][i] = (s0[i] + ptrs_[1][i]) * 0.5f;
            }
        } else {
            for (int c = 0; c < channels; ++c) std::memcpy(x[c], s0, bytes);
        }
    }

    // Plugins are not reset on the audio thread (VST3 reset is not real-time safe).
    void reset() override {}

    int latencySamples() const override { return plugin_->getLatencySamples(); }

    bool hasEditor() const override { return hasEditor_; }

    void setEditorOpen(bool open) override {
        std::weak_ptr<Instance> weak = shared_from_this();
        runOnMessageThread([weak, open] {
            if (auto self = weak.lock()) self->setEditorOpenOnMessageThread(open);
        });
    }

private:
    struct Param {
        AudioProcessorParameter* p;
        std::string key;
    };

    PluginParamInfo info(const Param& e) const {
        PluginParamInfo i;
        i.key = e.key;
        i.name = str(e.p->getName(128));
        i.unit = str(e.p->getLabel());
        i.value = e.p->getValue();
        i.defaultValue = e.p->getDefaultValue();
        const int steps = e.p->getNumSteps();
        i.steps = steps == AudioProcessor::getDefaultNumParameterSteps() ? 0 : steps;
        // getText talks to the plugin's controller (it takes the message manager lock itself)
        auto* p = e.p;
        i.text = onMessageThread<std::string>([p] { return str(p->getCurrentValueAsText()); }, std::string());
        return i;
    }

    void setEditorOpenOnMessageThread(bool open) {
        if (!open) {
            window_.reset();
            return;
        }
        if (window_) {
            window_->toFront(true);
            return;
        }
        if (!plugin_->hasEditor()) return notifyClosed();
        auto* editor = plugin_->createEditorAndMakeActive();
        if (editor == nullptr) return notifyClosed();
        std::weak_ptr<Instance> weak = shared_from_this();
        window_ = std::make_unique<EditorWindow>(plugin_->getName(), [weak] {
            // deleting the window from its own button callback is not allowed: do it next
            MessageManager::callAsync([weak] {
                if (auto self = weak.lock()) {
                    self->window_.reset();
                    self->notifyClosed();
                }
            });
        });
        window_->setUsingNativeTitleBar(true);
        window_->setContentOwned(editor, true);
        window_->setResizable(editor->isResizable(), false);
        window_->centreWithSize(window_->getWidth(), window_->getHeight());
        window_->setVisible(true);
        window_->toFront(true);
    }

    void notifyClosed() {
        std::lock_guard lock(listenerMutex_);
        if (listener_.onEditorClosed) listener_.onEditorClosed();
    }

    // AudioProcessorParameter::Listener: the plugin changed a value by itself (its window, a preset).
    void parameterValueChanged(int index, float value) override {
        auto it = byIndex_.find(index);
        if (it == byIndex_.end()) return;
        std::lock_guard lock(listenerMutex_);
        if (listener_.onParam) listener_.onParam(params_[it->second].key, value);
    }
    void parameterGestureChanged(int, bool) override {}

    std::unique_ptr<AudioPluginInstance> plugin_;
    int main_ = 2;  // channels of the plugin's main bus
    int total_ = 2; // channels of the buffer handed to processBlock
    std::string name_;
    bool hasEditor_ = false;
    std::vector<Param> params_;
    std::unordered_map<std::string, std::size_t> byKey_;
    std::unordered_map<int, std::size_t> byIndex_;
    std::array<std::vector<float>, kMaxChannels> scratch_;
    std::array<float*, kMaxChannels> ptrs_{};
    MidiBuffer midi_;
    std::mutex listenerMutex_;
    PluginInstance::Listener listener_;
    std::unique_ptr<EditorWindow> window_; // message thread
};

// ---------------------------------------------------------------------------------------------

struct CachedFile {
    std::string file;
    std::int64_t modified = 0;
    std::vector<PluginInfo> types;
    std::vector<std::string> descriptions; // PluginDescription XML per type ("" when not loadable)
};

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

var typeToVar(const PluginInfo& t, const std::string& desc) {
    auto o = std::make_unique<DynamicObject>();
    o->setProperty("id", String(t.id));
    o->setProperty("name", String::fromUTF8(t.name.c_str()));
    o->setProperty("vendor", String::fromUTF8(t.vendor.c_str()));
    o->setProperty("category", String::fromUTF8(t.category.c_str()));
    o->setProperty("instrument", t.instrument);
    o->setProperty("status", String(t.status));
    o->setProperty("error", String::fromUTF8(t.error.c_str()));
    o->setProperty("desc", String::fromUTF8(desc.c_str()));
    return var(o.release());
}

PluginInfo typeFromVar(const var& v, const std::string& file, std::string* desc) {
    PluginInfo t;
    t.id = str(v["id"].toString());
    t.name = str(v["name"].toString());
    t.vendor = str(v["vendor"].toString());
    t.category = str(v["category"].toString());
    t.instrument = static_cast<bool>(v["instrument"]);
    t.status = str(v["status"].toString());
    t.error = str(v["error"].toString());
    t.path = file;
    if (desc) *desc = str(v["desc"].toString());
    return t;
}

class Host final : public PluginHost {
public:
    explicit Host(HostOptions o) : options_(std::move(o)) {
        formats_.addFormat(std::make_unique<VST3PluginFormat>());
        cacheFile_ = options_.cacheFile.empty()
                         ? File::getSpecialLocation(File::userApplicationDataDirectory)
                               .getChildFile("DesktopCapture")
                               .getChildFile("vst3-cache.json")
                         : File(String::fromUTF8(options_.cacheFile.c_str()));
        scanner_ = options_.scannerExe.empty() ? File::getSpecialLocation(File::currentExecutableFile)
                                               : File(String::fromUTF8(options_.scannerExe.c_str()));
        loadCache();
    }

    ~Host() override {
        alive_.reset();
        stopScan_ = true;
        if (scanThread_.joinable()) scanThread_.join();
    }

    std::vector<PluginInfo> plugins() const override {
        std::lock_guard lock(mutex_);
        std::vector<PluginInfo> out;
        for (const auto& f : files_)
            for (const auto& t : f.types) out.push_back(t);
        return out;
    }

    bool scan(bool rescan, const std::vector<std::string>& extraDirs, std::function<void(ScanProgress)> progress,
              std::function<void()> done) override {
        if (scanning_.exchange(true)) return false;
        if (scanThread_.joinable()) scanThread_.join();
        scanThread_ = std::thread([this, rescan, extraDirs, progress = std::move(progress), done = std::move(done)] {
            runScan(rescan, extraDirs, progress);
            scanning_ = false;
            done();
        });
        return true;
    }

    void create(const std::string& id,
                std::function<void(std::shared_ptr<PluginInstance>, std::string error)> done) override {
        PluginDescription desc;
        bool found = false;
        {
            std::lock_guard lock(mutex_);
            for (const auto& f : files_)
                for (std::size_t i = 0; i < f.types.size(); ++i)
                    if (f.types[i].id == id && !f.descriptions[i].empty())
                        if (auto xml = parseXML(String::fromUTF8(f.descriptions[i].c_str())))
                            found = desc.loadFromXml(*xml);
        }
        if (!found) return done(nullptr, "plugin " + id + " is not in the scan results");
        std::weak_ptr<int> alive = alive_;
        runOnMessageThread([this, alive, desc, done] {
            if (alive.expired()) return done(nullptr, "engine is shutting down");
            formats_.createPluginInstanceAsync(desc, kRate, kFxMaxFrames,
                                               [done](std::unique_ptr<AudioPluginInstance> plugin, const String& error) {
                                                   if (!plugin) return done(nullptr, str(error.isEmpty() ? "could not load" : error));
                                                   const int main = setupLayout(*plugin);
                                                   if (main == 0) return done(nullptr, "no mono or stereo main bus");
                                                   done(std::make_shared<Instance>(std::move(plugin), main), "");
                                               });
        });
    }

private:
    void loadCache() {
        const auto root = JSON::parse(cacheFile_);
        auto* files = root["files"].getArray();
        if (files == nullptr) return;
        std::lock_guard lock(mutex_);
        for (const auto& f : *files) {
            CachedFile c;
            c.file = str(f["file"].toString());
            c.modified = f["modified"].toString().getLargeIntValue();
            if (auto* types = f["types"].getArray())
                for (const auto& t : *types) {
                    std::string desc;
                    c.types.push_back(typeFromVar(t, c.file, &desc));
                    c.descriptions.push_back(desc);
                }
            files_.push_back(std::move(c));
        }
    }

    void saveCache(const std::vector<CachedFile>& files) {
        Array<var> list;
        for (const auto& f : files) {
            auto o = std::make_unique<DynamicObject>();
            o->setProperty("file", String::fromUTF8(f.file.c_str()));
            o->setProperty("modified", static_cast<int64>(f.modified));
            Array<var> types;
            for (std::size_t i = 0; i < f.types.size(); ++i) types.add(typeToVar(f.types[i], f.descriptions[i]));
            o->setProperty("types", types);
            list.add(var(o.release()));
        }
        auto root = std::make_unique<DynamicObject>();
        root->setProperty("version", 1);
        root->setProperty("files", list);
        cacheFile_.getParentDirectory().createDirectory();
        cacheFile_.replaceWithText(JSON::toString(var(root.release())));
    }

    // A file that could not be read gets one entry named after the file, so the list shows it.
    static CachedFile failedFile(const std::string& file, std::int64_t modified, const std::string& status, const String& error) {
        CachedFile c;
        c.file = file;
        c.modified = modified;
        PluginInfo t;
        t.name = str(File(String::fromUTF8(file.c_str())).getFileNameWithoutExtension());
        t.path = file;
        t.status = status;
        t.error = str(error);
        c.types.push_back(t);
        c.descriptions.push_back({});
        return c;
    }

    CachedFile scanFile(const String& file, std::int64_t modified) {
        const auto tmp = File::getSpecialLocation(File::tempDirectory).getChildFile("dc-mixd-scan");
        tmp.createDirectory();
        const auto json = tmp.getChildFile("scan-" + String(Time::currentTimeMillis()) + ".json");
        ChildProcess child;
        if (!child.start(StringArray{scanner_.getFullPathName(), "--scan-plugin", file, "--out", json.getFullPathName()}, 0))
            return failedFile(str(file), modified, "failed", "could not start the scanner process");
        const bool finished = child.waitForProcessToFinish(options_.scanTimeoutSeconds * 1000);
        if (!finished) {
            child.kill();
            json.deleteFile();
            return failedFile(str(file), modified, "timeout",
                              "did not load within " + String(options_.scanTimeoutSeconds) + " s");
        }
        const auto code = child.getExitCode();
        const auto result = JSON::parse(json);
        json.deleteFile();
        if (!result.isObject()) return failedFile(str(file), modified, "crashed", "scanner died: " + exitDescription(code));
        CachedFile c;
        c.file = str(file);
        c.modified = modified;
        if (auto* types = result["types"].getArray())
            for (const auto& t : *types) {
                std::string desc;
                c.types.push_back(typeFromVar(t, c.file, &desc));
                c.descriptions.push_back(desc);
            }
        if (c.types.empty()) return failedFile(str(file), modified, "failed", result["error"].toString());
        return c;
    }

    void runScan(bool rescan, const std::vector<std::string>& extraDirs, const std::function<void(ScanProgress)>& progress) {
        VST3PluginFormat format;
        auto path = options_.standardDirs ? format.getDefaultLocationsToSearch() : FileSearchPath();
        for (const auto& d : extraDirs) path.add(File(String::fromUTF8(d.c_str())));
        auto bundles = format.searchPathsForPlugins(path, true, false);
        bundles.sort(true);

        std::vector<CachedFile> previous;
        {
            std::lock_guard lock(mutex_);
            previous = files_;
        }
        std::vector<CachedFile> result;
        for (int i = 0; i < bundles.size() && !stopScan_; ++i) {
            const auto& file = bundles[i];
            progress({static_cast<double>(i) / bundles.size(), str(file)});
            const auto modified = File(file).getLastModificationTime().toMilliseconds();
            auto it = std::find_if(previous.begin(), previous.end(),
                                   [&](const CachedFile& c) { return c.file == str(file) && c.modified == modified; });
            if (!rescan && it != previous.end()) {
                result.push_back(*it);
                continue;
            }
            if (options_.verbose) std::cout << "scanning " << file << std::endl;
            result.push_back(scanFile(file, modified));
            if (options_.verbose)
                for (const auto& t : result.back().types) std::cout << "  " << t.status << "  " << t.name << " " << t.error << std::endl;
        }
        if (stopScan_) return;
        saveCache(result);
        std::lock_guard lock(mutex_);
        files_ = std::move(result);
    }

    HostOptions options_;
    AudioPluginFormatManager formats_; // message thread (createPluginInstanceAsync)
    File cacheFile_, scanner_;
    mutable std::mutex mutex_;
    std::vector<CachedFile> files_;
    std::atomic<bool> scanning_{false}, stopScan_{false};
    std::thread scanThread_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

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

} // namespace

std::unique_ptr<PluginHost> makeHost(const HostOptions& options) { return std::make_unique<Host>(options); }

int runWithMessageLoop(const std::function<int()>& body) {
    ScopedJuceInitialiser_GUI juce;
    auto* mm = MessageManager::getInstance();
    std::atomic<int> rc{0};
    std::thread worker([&] {
        rc = body();
        mm->stopDispatchLoop();
    });
    mm->runDispatchLoop();
    worker.join(); // what the worker posted on its way out (plugin deletions) ran before the quit message
    return rc;
}

int scanChildMain(const std::string& fileUtf8, const std::string& outUtf8) {
    disableCrashDialogs();
    ScopedJuceInitialiser_GUI juce;
    const String file = String::fromUTF8(fileUtf8.c_str());
    AudioPluginFormatManager fm;
    fm.addFormat(std::make_unique<VST3PluginFormat>());
    OwnedArray<PluginDescription> types;
    fm.getFormat(0)->findAllTypesForFile(types, file);

    Array<var> list;
    for (auto* desc : types) {
        PluginInfo t;
        t.id = idFor(*desc);
        t.name = str(desc->name);
        t.vendor = str(desc->manufacturerName);
        t.category = str(desc->category);
        t.instrument = desc->isInstrument;
        String error;
        auto plugin = fm.createPluginInstance(*desc, kRate, kFxMaxFrames, error);
        if (!plugin) {
            t.status = "failed";
            t.error = str(error.isEmpty() ? String("could not load") : error);
        } else if (setupLayout(*plugin) == 0) {
            t.status = "unsupported";
            t.error = "no mono or stereo main bus";
        }
        plugin.reset();
        std::string xml;
        if (t.status == "ok")
            if (auto x = desc->createXml()) xml = str(x->toString(XmlElement::TextFormat().singleLine().withoutHeader()));
        list.add(typeToVar(t, xml));
    }
    auto root = std::make_unique<DynamicObject>();
    root->setProperty("file", file);
    root->setProperty("types", list);
    if (types.isEmpty()) root->setProperty("error", "no plugin types found in the file");
    const File out(String::fromUTF8(outUtf8.c_str()));
    out.getParentDirectory().createDirectory();
    return out.replaceWithText(JSON::toString(var(root.release()))) ? 0 : 1;
}

} // namespace dc::mixd::vst3
