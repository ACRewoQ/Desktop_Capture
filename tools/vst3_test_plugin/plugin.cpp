// DC Test Gain: known-good VST3 fixture for dc-vst3-probe (see CMakeLists.txt for the env switches).

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <thread>

namespace {

bool envFlag(const char* name) {
    return juce::SystemStats::getEnvironmentVariable(name, {}).getIntValue() != 0;
}

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout() {
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID{"gain", 1}, "Gain",
                                                     NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f,
                                                     AudioParameterFloatAttributes().withLabel("dB")));
    layout.add(std::make_unique<AudioParameterBool>(ParameterID{"mute", 1}, "Mute", false));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID{"mode", 1}, "Mode",
                                                      StringArray{"Clean", "Soft clip", "Hard clip"}, 0));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID{"steps", 1}, "Steps", 1, 8, 4));
    return layout;
}

class TestEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    TestEditor(juce::AudioProcessor& p, juce::RangedAudioParameter* gain)
        : AudioProcessorEditor(p), generic_(p), gain_(gain) {
        addAndMakeVisible(generic_);
        setResizable(true, true);
        setResizeLimits(320, 200, 1200, 800);
        setSize(420, 260);
        if (envFlag("DC_TEST_PLUGIN_GUI_EDITS"))
            startTimer(100);
    }

    void resized() override { generic_.setBounds(getLocalBounds()); }

private:
    // Behaves like a user dragging the knob: a gesture with one edit, reported to the host.
    void timerCallback() override {
        tick_ = (tick_ + 1) % 10;
        gain_->beginChangeGesture();
        gain_->setValueNotifyingHost(0.5f + 0.04f * static_cast<float>(tick_));
        gain_->endChangeGesture();
    }

    juce::GenericAudioProcessorEditor generic_;
    juce::RangedAudioParameter* gain_;
    int tick_ = 0;
};

class TestGainProcessor final : public juce::AudioProcessor {
public:
    TestGainProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          state_(*this, nullptr, "DcTestGain", makeLayout()) {
        if (envFlag("DC_TEST_PLUGIN_CRASH")) {
            volatile int* p = nullptr;
            *p = 1;
        }
        if (envFlag("DC_TEST_PLUGIN_HANG"))
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));

        gain_ = state_.getRawParameterValue("gain");
        mute_ = state_.getRawParameterValue("mute");
        mode_ = state_.getRawParameterValue("mode");
    }

    const juce::String getName() const override { return "DC Test Gain"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
        const auto& out = layouts.getMainOutputChannelSet();
        if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
            return false;
        return layouts.getMainInputChannelSet() == out;
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        const float g = *mute_ > 0.5f ? 0.0f : juce::Decibels::decibelsToGain(gain_->load());
        const int mode = static_cast<int>(mode_->load());
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
            auto* d = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i) {
                float s = d[i] * g;
                if (mode == 1)
                    s = std::tanh(s);
                else if (mode == 2)
                    s = juce::jlimit(-1.0f, 1.0f, s);
                d[i] = s;
            }
        }
    }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override {
        return new TestEditor(*this, state_.getParameter("gain"));
    }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& dest) override {
        if (auto xml = state_.copyState().createXml())
            copyXmlToBinary(*xml, dest);
    }
    void setStateInformation(const void* data, int size) override {
        if (auto xml = getXmlFromBinary(data, size))
            if (xml->hasTagName(state_.state.getType()))
                state_.replaceState(juce::ValueTree::fromXml(*xml));
    }

private:
    juce::AudioProcessorValueTreeState state_;
    std::atomic<float>* gain_ = nullptr;
    std::atomic<float>* mute_ = nullptr;
    std::atomic<float>* mode_ = nullptr;
};

} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestGainProcessor(); }
