// The video mixer settings as capd uses them (sources, scenes with layers, mixes), read from the engine's tree
// (/video/source/#, /video/scene/#, /video/mix/#; docs/spec/protocol.md 4.5), and the parts of mixing that need no
// GPU: where a layer lands on the canvas, the state of a scene transition, which sources a mix needs.
// Portable: no Windows types, tested on Linux.
#pragma once

#include "dc/protocol/param_tree.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dc::capd {

struct TextStyle {
    std::string content;
    std::string font = "Yu Gothic UI";
    int size = 48;                       // pixels
    std::uint32_t color = 0xFFFFFFFF;    // 0xAARRGGBB
    std::uint32_t background = 0;        // 0xAARRGGBB
    bool bold = false;
    bool italic = false;
    std::string align = "left";          // left | center | right
    int wrapWidth = 0;                   // pixels, 0 = no wrapping
    bool operator==(const TextStyle&) const = default;
};

struct SourceConfig {
    int id = 0;
    std::string kind = "display";  // display | window | camera | image | media | text | color | lan
    bool cursor = true;
    std::string displayId;         // "" = primary
    std::string displayMethod = "wgc";
    std::string windowExe, windowTitle;
    std::string windowMatch = "title";  // title (exe + title) | exe
    std::string cameraId, cameraName;
    int cameraWidth = 0, cameraHeight = 0, cameraFps = 0;
    std::string file;
    bool mediaLoop = true;
    TextStyle text;
    std::uint32_t color = 0xFF000000;
    std::string lanStream;
    // Everything that needs the source to be opened again when it changes (all of it: reopening is cheap
    // compared with keeping per-field update paths, and settings change rarely).
    bool operator==(const SourceConfig&) const = default;
};

struct LayerConfig {
    int id = 0;
    int source = 0;
    bool visible = true;
    double x = 0, y = 0, width = 1, height = 1;  // canvas = 1
    std::string fit = "fit";                     // fit | fill | stretch
    double cropLeft = 0, cropTop = 0, cropRight = 0, cropBottom = 0;
    double opacity = 1;
    bool chroma = false;
    std::uint32_t chromaColor = 0x00FF00;  // 0xRRGGBB
    double similarity = 0.4, smoothness = 0.08, spill = 0.1;
    bool noSignalImage = false;
    bool operator==(const LayerConfig&) const = default;
};

struct SceneConfig {
    int id = 0;
    std::string name;
    std::vector<LayerConfig> layers;  // back to front (drawing order)
    bool operator==(const SceneConfig&) const = default;
};

struct MixConfig {
    int id = 0;
    std::string name;
    int width = 1920, height = 1080, fps = 60;
    std::uint32_t background = 0xFF000000;
    int programScene = 0, previewScene = 0;
    std::string transition = "fade";  // cut | fade
    int transitionMs = 300;
    bool operator==(const MixConfig&) const = default;
};

struct VideoConfig {
    std::map<int, SourceConfig> sources;
    std::map<int, SceneConfig> scenes;
    std::map<int, MixConfig> mixes;
    bool operator==(const VideoConfig&) const = default;
};

VideoConfig videoConfigFromTree(const proto::ParamTree& tree);

// Where a layer lands: destination rectangle in canvas pixels and the part of the source texture it shows
// (0..1 texture coordinates, after cropping and, for "fill", cutting what falls outside the box).
struct LayerGeometry {
    float x = 0, y = 0, width = 0, height = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    bool empty() const { return width <= 0 || height <= 0 || u1 <= u0 || v1 <= v0; }
};

// srcW / srcH = 0 means the source has no natural size (single colour): the box is filled.
LayerGeometry layoutLayer(const LayerConfig& layer, int srcW, int srcH, int canvasW, int canvasH);

// Follows a mix's program scene and says what to draw. A change of program starts a transition from the scene
// shown so far; a change during a running transition fades from a frozen copy of the current picture instead
// (freezeNow is set for the one frame where that copy must be taken).
class TransitionTracker {
public:
    struct State {
        int from = 0;        // scene fading out (0 = none / black); ignored when fromFrozen
        int to = 0;          // scene fading in (or simply shown when progress = 1)
        double progress = 1; // 0..1
        bool fromFrozen = false;
        bool freezeNow = false;
        bool active() const { return progress < 1.0; }
    };

    State update(const MixConfig& mix, std::int64_t nowMs);
    void reset() { initialised_ = false; }

private:
    bool initialised_ = false;
    int from_ = 0, to_ = 0;
    bool fromFrozen_ = false;
    bool freezePending_ = false;
    std::int64_t startMs_ = 0;
    int durationMs_ = 0;
};

// Sources a running mix keeps open: every layer of its program and preview scenes and of the scene it is
// fading from, so that a take or a visibility toggle shows a picture at once.
void addNeededSources(const VideoConfig& cfg, const MixConfig& mix, const TransitionTracker::State& t, std::set<int>& out);

// "0xAARRGGBB" helpers: components 0..1.
struct Rgba {
    float r = 0, g = 0, b = 0, a = 1;
};
Rgba argbToRgba(std::uint32_t argb);

}  // namespace dc::capd
