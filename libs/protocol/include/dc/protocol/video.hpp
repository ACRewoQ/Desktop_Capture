#pragma once

// The video mixer part of the parameter tree (/video/source, /video/scene, /video/mix): the methods
// that add, remove and reorder sources, scenes, layers and mixes, the take (transition), capd's
// reports, the checks on references and the derived /video/mix/#/active flags.
// Pure functions on a ParamTree, shared by the engine (mixd) and the mock server so both behave the same.

#include "dc/protocol/messages.hpp"
#include "dc/protocol/param_tree.hpp"
#include "dc/protocol/schema.hpp"

#include <string>
#include <vector>

namespace dc::proto {

// What a video method wants applied: removed subtrees first, then changes (like a Delta), plus the
// Result to return. When result.code != Ok nothing must be applied. event is set (non-empty name) when
// the method asks for a broadcast instead (/video/devices/refresh).
struct VideoOutcome {
    Result result;
    std::vector<Change> changes;
    std::vector<std::string> removed;
    Event event;
};

// Method patterns handled by videoCall(), for ControlServer::registerMethod.
const std::vector<std::string>& videoMethodPatterns();

// Runs one video method against the current tree. role is the caller's (reports are Service only).
VideoOutcome videoCall(const ParamTree& tree, const std::string& method, const Args& args, ClientRole role);

// Default tree of a new source / scene / layer (without position, which the caller assigns).
std::vector<Change> videoSourceDefaults(int source, const std::string& kind, const Schema& schema = schemaV1());
std::vector<Change> videoSceneDefaults(int scene, const Schema& schema = schemaV1());
std::vector<Change> videoLayerDefaults(int scene, int layer, int source, const Schema& schema = schemaV1());
// Defaults of mix n (all /video/mix/#/... values; name "Mix A".."Mix D").
std::vector<Change> videoMixDefaults(int mix, const Schema& schema = schemaV1());

// What a fresh engine starts with besides the mix list: source 1 = the primary display, scene 1 with
// one full-canvas layer showing it. (globalDefaults() includes these and points mix 1 at scene 1.)
std::vector<Change> videoStartDefaults(const Schema& schema = schemaV1());

// Checks the video references after a client Set: layer sources, program/preview scenes and the
// outputs' mix numbers must exist (or be 0). Part of defaultSemanticValidator().
ValidationError validateVideo(const ParamTree& after, const std::vector<Change>& changes);

// /video/mix/#/active for every existing mix: true while an output refers to it. The engine publishes
// these after any change (unchanged values make no delta).
std::vector<Change> videoDerived(const ParamTree& tree);

// Helpers for both sides.
std::vector<int> videoScenesByPosition(const ParamTree& tree);
std::vector<int> videoLayersByPosition(const ParamTree& tree, int scene);  // front (position 1) first
std::vector<int> videoSources(const ParamTree& tree);                      // ascending number
std::vector<int> videoMixes(const ParamTree& tree);                        // ascending number

} // namespace dc::proto
