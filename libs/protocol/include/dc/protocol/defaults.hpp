#pragma once

#include "dc/protocol/messages.hpp"
#include "dc/protocol/schema.hpp"

#include <string>
#include <vector>

namespace dc::proto {

// Default parameter sets derived from the schema, so the engine and the mock server start from
// identical trees. Indexed lists (out/#, send/#, audio/#, mix/#) are expanded to their fixed sizes;
// fx slots and device lists are not included (they appear when added or detected).

// All /track/N/... parameters except position (the caller assigns it). Output 1 targets "master".
std::vector<Change> trackDefaults(int track, const Schema& schema = schemaV1());

// All /master/... parameters.
std::vector<Change> masterDefaults(const Schema& schema = schemaV1());

// Everything else that is not per-track, per-device or per-process: /system, /mixer, /cue,
// /capture, /video, /remote.
std::vector<Change> globalDefaults(const Schema& schema = schemaV1());

// Number of entries of an indexed list, keyed by the segment before "#" ("out" -> 8).
int listSize(const std::string& segment);

} // namespace dc::proto
