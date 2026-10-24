#include "video_config.hpp"

#include "dc/protocol/video.hpp"

#include <algorithm>

namespace dc::capd {

namespace {

struct Reader {
    const proto::ParamTree& tree;
    std::string base;

    void str(const char* key, std::string& out) const {
        if (auto v = tree.getAs<std::string>(base + key)) out = *v;
    }
    void boolean(const char* key, bool& out) const {
        if (auto v = tree.getAs<bool>(base + key)) out = *v;
    }
    void integer(const char* key, int& out) const {
        if (auto v = tree.getAs<std::int64_t>(base + key)) out = static_cast<int>(*v);
    }
    void color(const char* key, std::uint32_t& out) const {
        if (auto v = tree.getAs<std::int64_t>(base + key)) out = static_cast<std::uint32_t>(*v);
    }
    void real(const char* key, double& out) const {
        if (auto v = tree.getAs<double>(base + key)) out = *v;
    }
};

}  // namespace

VideoConfig videoConfigFromTree(const proto::ParamTree& tree) {
    VideoConfig cfg;
    for (int id : proto::videoSources(tree)) {
        SourceConfig s;
        s.id = id;
        const Reader r{tree, "/video/source/" + std::to_string(id)};
        r.str("/kind", s.kind);
        r.boolean("/cursor", s.cursor);
        r.str("/display/id", s.displayId);
        r.str("/display/method", s.displayMethod);
        r.str("/window/exe", s.windowExe);
        r.str("/window/title", s.windowTitle);
        r.str("/window/match", s.windowMatch);
        r.str("/camera/id", s.cameraId);
        r.str("/camera/name", s.cameraName);
        r.integer("/camera/width", s.cameraWidth);
        r.integer("/camera/height", s.cameraHeight);
        r.integer("/camera/fps", s.cameraFps);
        r.str("/file", s.file);
        r.boolean("/media/loop", s.mediaLoop);
        r.str("/text/content", s.text.content);
        r.str("/text/font", s.text.font);
        r.integer("/text/size", s.text.size);
        r.color("/text/color", s.text.color);
        r.color("/text/background", s.text.background);
        r.boolean("/text/bold", s.text.bold);
        r.boolean("/text/italic", s.text.italic);
        r.str("/text/align", s.text.align);
        r.integer("/text/wrap_width", s.text.wrapWidth);
        r.color("/color/value", s.color);
        r.str("/lan/stream", s.lanStream);
        cfg.sources[id] = std::move(s);
    }
    for (int id : proto::videoScenesByPosition(tree)) {
        SceneConfig sc;
        sc.id = id;
        Reader{tree, "/video/scene/" + std::to_string(id)}.str("/name", sc.name);
        auto layers = proto::videoLayersByPosition(tree, id);  // front first
        std::reverse(layers.begin(), layers.end());
        for (int lid : layers) {
            LayerConfig l;
            l.id = lid;
            const Reader r{tree, "/video/scene/" + std::to_string(id) + "/layer/" + std::to_string(lid)};
            r.integer("/source", l.source);
            r.boolean("/visible", l.visible);
            r.real("/x", l.x);
            r.real("/y", l.y);
            r.real("/width", l.width);
            r.real("/height", l.height);
            r.str("/fit", l.fit);
            r.real("/crop/left", l.cropLeft);
            r.real("/crop/top", l.cropTop);
            r.real("/crop/right", l.cropRight);
            r.real("/crop/bottom", l.cropBottom);
            r.real("/opacity", l.opacity);
            r.boolean("/chroma/enabled", l.chroma);
            r.color("/chroma/color", l.chromaColor);
            r.real("/chroma/similarity", l.similarity);
            r.real("/chroma/smoothness", l.smoothness);
            r.real("/chroma/spill", l.spill);
            std::string noSignal;
            r.str("/no_signal", noSignal);
            l.noSignalImage = noSignal == "image";
            sc.layers.push_back(l);
        }
        cfg.scenes[id] = std::move(sc);
    }
    for (int id : proto::videoMixes(tree)) {
        MixConfig m;
        m.id = id;
        const Reader r{tree, "/video/mix/" + std::to_string(id)};
        r.str("/name", m.name);
        r.integer("/width", m.width);
        r.integer("/height", m.height);
        r.integer("/fps", m.fps);
        r.color("/background", m.background);
        r.integer("/program_scene", m.programScene);
        r.integer("/preview_scene", m.previewScene);
        r.str("/transition/type", m.transition);
        r.integer("/transition/duration_ms", m.transitionMs);
        m.width = std::max(16, m.width & ~1);
        m.height = std::max(16, m.height & ~1);
        m.fps = std::max(1, m.fps);
        cfg.mixes[id] = std::move(m);
    }
    return cfg;
}

LayerGeometry layoutLayer(const LayerConfig& l, int srcW, int srcH, int canvasW, int canvasH) {
    LayerGeometry g;
    const double bx = l.x * canvasW, by = l.y * canvasH, bw = l.width * canvasW, bh = l.height * canvasH;
    const double u0 = std::clamp(l.cropLeft, 0.0, 1.0), u1 = 1.0 - std::clamp(l.cropRight, 0.0, 1.0);
    const double v0 = std::clamp(l.cropTop, 0.0, 1.0), v1 = 1.0 - std::clamp(l.cropBottom, 0.0, 1.0);
    if (bw <= 0 || bh <= 0 || u1 <= u0 || v1 <= v0) {
        g.width = g.height = 0;
        return g;
    }
    g.u0 = float(u0), g.u1 = float(u1), g.v0 = float(v0), g.v1 = float(v1);
    g.x = float(bx), g.y = float(by), g.width = float(bw), g.height = float(bh);
    if (srcW <= 0 || srcH <= 0 || l.fit == "stretch") return g;

    const double cw = srcW * (u1 - u0), ch = srcH * (v1 - v0);
    if (l.fit == "fill") {
        // keep the aspect ratio and cover the box: cut the overflow symmetrically from the source
        const double scale = std::max(bw / cw, bh / ch);
        const double fu = (bw / scale) / cw, fv = (bh / scale) / ch;  // visible part of the cropped source, <= 1
        const double cu = (u0 + u1) / 2, cv = (v0 + v1) / 2;
        g.u0 = float(cu - (u1 - u0) * fu / 2), g.u1 = float(cu + (u1 - u0) * fu / 2);
        g.v0 = float(cv - (v1 - v0) * fv / 2), g.v1 = float(cv + (v1 - v0) * fv / 2);
        return g;
    }
    // fit: the whole cropped source inside the box, centred
    const double scale = std::min(bw / cw, bh / ch);
    g.width = float(cw * scale), g.height = float(ch * scale);
    g.x = float(bx + (bw - cw * scale) / 2), g.y = float(by + (bh - ch * scale) / 2);
    return g;
}

TransitionTracker::State TransitionTracker::update(const MixConfig& mix, std::int64_t nowMs) {
    if (!initialised_) {
        initialised_ = true;
        from_ = to_ = mix.programScene;
        fromFrozen_ = freezePending_ = false;
        durationMs_ = 0;
    }
    auto progressAt = [&](std::int64_t t) {
        return durationMs_ > 0 ? std::clamp(double(t - startMs_) / double(durationMs_), 0.0, 1.0) : 1.0;
    };
    if (mix.programScene != to_) {
        const bool cut = mix.transition == "cut" || mix.transitionMs <= 0;
        if (cut) {
            from_ = mix.programScene;
            fromFrozen_ = false;
            durationMs_ = 0;
        } else {
            if (progressAt(nowMs) < 1.0) {
                fromFrozen_ = true;  // the picture of the interrupted transition
                freezePending_ = true;
            } else {
                from_ = to_;
                fromFrozen_ = false;
            }
            durationMs_ = mix.transitionMs;
        }
        to_ = mix.programScene;
        startMs_ = nowMs;
    }
    State s;
    s.progress = progressAt(nowMs);
    if (s.progress >= 1.0) {
        from_ = to_;
        fromFrozen_ = false;
        freezePending_ = false;
    }
    s.from = from_;
    s.to = to_;
    s.fromFrozen = fromFrozen_;
    s.freezeNow = freezePending_;
    freezePending_ = false;
    return s;
}

void addNeededSources(const VideoConfig& cfg, const MixConfig& mix, const TransitionTracker::State& t, std::set<int>& out) {
    auto addScene = [&](int id) {
        auto it = cfg.scenes.find(id);
        if (it == cfg.scenes.end()) return;
        for (const auto& l : it->second.layers)
            if (l.source != 0 && cfg.sources.count(l.source)) out.insert(l.source);
    };
    addScene(mix.programScene);
    addScene(mix.previewScene);
    addScene(t.to);
    if (t.active() && !t.fromFrozen) addScene(t.from);
}

Rgba argbToRgba(std::uint32_t argb) {
    Rgba c;
    c.a = float((argb >> 24) & 0xFF) / 255.0f;
    c.r = float((argb >> 16) & 0xFF) / 255.0f;
    c.g = float((argb >> 8) & 0xFF) / 255.0f;
    c.b = float(argb & 0xFF) / 255.0f;
    return c;
}

}  // namespace dc::capd
