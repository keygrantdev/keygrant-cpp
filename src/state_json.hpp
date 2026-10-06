// StoredState as JSON: the reference's field names, absent fields omitted.
#pragma once

#include "json_util.hpp"
#include "keygrant/types.hpp"

namespace keygrant::detail {

/// The state as a JSON object: the known fields in a fixed order, then the ones kept unread.
ordered_json stateToJson(const StoredState& state);

/// A state from a JSON object. A known field of the wrong type is read as absent; unknown fields are kept.
StoredState stateFromJson(const json& object);

}  // namespace keygrant::detail
