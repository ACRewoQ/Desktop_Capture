#pragma once

#include "dc/protocol/messages.hpp"
#include "dc/protocol/param_tree.hpp"
#include "dc/protocol/schema.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dc::proto {

// Server-side owner of the parameter tree. Validates client changes against the schema, applies
// them atomically, and produces the Delta to broadcast. Not thread-safe: the control server calls it
// from its network thread only.
class ParamStore {
public:
    // Extra checks that need the whole tree (e.g. routing loops). Called after a client Set has been
    // applied tentatively; returning an error rolls the Set back.
    using SemanticValidator = std::function<ValidationError(const ParamTree& after, const std::vector<Change>& changes)>;

    explicit ParamStore(const Schema& schema = schemaV1());

    const ParamTree& tree() const { return tree_; }
    const Schema& schema() const { return schema_; }
    Snapshot snapshot() const { return tree_.toSnapshot(); }

    void setSemanticValidator(SemanticValidator v) { validator_ = std::move(v); }

    struct Outcome {
        Result result;
        std::optional<Delta> delta; // empty when nothing changed
    };

    // Applies a client Set: every change must name an existing path, pass the schema (including the
    // read-only check) and the semantic validator, otherwise nothing is applied.
    Outcome applySet(const Set& set, const std::string& clientName);

    // Engine-side change (device list, meters ids, recording state, track add/remove...).
    // Skips the read-only check, still checks types against the schema. Paths may be new.
    // Throws std::invalid_argument on schema violations: that is a bug in the caller.
    std::optional<Delta> publish(const std::vector<Change>& changes, const std::vector<std::string>& removed = {},
                                 std::uint64_t originId = 0, const std::string& originClient = {});

private:
    const Schema& schema_;
    ParamTree tree_;
    SemanticValidator validator_;
};

// Default semantic validator: runs validateRouting() when a routing target changed and
// validateFxChanges() for effect parameters.
ValidationError defaultSemanticValidator(const ParamTree& after, const std::vector<Change>& changes);

} // namespace dc::proto
