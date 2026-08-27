#pragma once

#include "common/Value.h"
#include "common/ResultOrError.hpp"

#include "common/Error.h"

#include <string>

namespace pp::common::io {

/** Serialize any Value root to JSON (UTF-8). indent < 0 → compact. */
pp::Roe<std::string> valueToJsonString(const Value &v, int indent = -1);

/** Parse JSON text into a Value (object, array, scalar, or null). */
pp::Roe<Value> valueFromJsonString(const std::string &json);

/**
 * Serialize Object to JSON.
 * indent < 0 → compact; indent >= 0 → pretty-print.
 * On encode failure (e.g. u64 > INT64_MAX), returns a small JSON error object.
 */
std::string objectToJsonString(const Object &o, int indent = -1);

/**
 * Parse a JSON object into Object. Returns false on syntax/type mismatch or
 * non-object root. Prefer valueFromJsonString for structured errors.
 */
bool objectFromJsonString(Object &out, const std::string &json);

/** @deprecated Prefer objectToJsonString — kept for ledger Meta call sites. */
inline std::string metaToJsonString(const Object &o, int indent = -1) {
  return objectToJsonString(o, indent);
}

/** @deprecated Prefer objectFromJsonString. */
inline bool metaFromJsonString(Object &out, const std::string &json) {
  return objectFromJsonString(out, json);
}

} // namespace pp::common::io
