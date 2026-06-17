/*
 * Copyright 2020 Yohan Pipereau
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _UTILS_H
#define _UTILS_H

#include <chrono>
#include <string>

#include <grpcpp/grpcpp.h>

using std::chrono::system_clock;
using std::chrono::duration_cast;
using std::chrono::nanoseconds;

/* Get current time since epoch in nanosec.
 * Returns int64_t to match gNMI Notification.timestamp / Leaf.collectedNs
 * (spec §2.2.1 mandates a signed 64-bit value), avoiding scattered casts. */
inline int64_t get_time_nanosec()
{
  nanoseconds ts;
  ts = duration_cast<nanoseconds>(system_clock::now().time_since_epoch());
  return ts.count();
}

/* gNMI Path → xpath string (structural only — origin is NOT embedded)
 * e.g. {elem:[{name:"components"},{name:"component",key:{name:"PSC-0"}}]}
 *   →  "/components/component[name=\"PSC-0\"]"
 *
 * origin is a protocol-boundary concern (D16 single-origin core): it is
 * validated and stripped by validateOrigin before a path reaches routing, so
 * this converter must stay origin-agnostic. Baking origin in here made an
 * origin-bearing path (incl. the canonical "openconfig") miss routing — C1.
 *
 * INVARIANT (backlog R3): every RPC path that reaches Backend routing MUST first
 * pass through validateOrigin() — this converter silently drops origin and does
 * not enforce it. There is no single choke point yet (get/set/subscribe each
 * validate at their boundary); the push Resolver (protocol-layer-design.md
 * pipeline step 1) is the intended single home.
 */
inline std::string gnmi_to_xpath(const gnmi::Path& path)
{
  std::string str;

  if (path.elem_size() <= 0)
    return str;

  for (auto& node : path.elem()) {
    str += "/";
    str += node.name();
    for (auto key : node.key())
      str += "[" + key.first + "=\"" + key.second + "\"]";
  }

  return str;
}

/* xpath string → gNMI Path (fills existing Path message)
 * e.g. "/components/component[name=PSC-0]/state/temperature/instant"
 *   →  PathElems with correct name + key fields
 *
 * Handles both [key=value] and [key="value"] quoting.
 */
inline void xpath_to_gnmi_path(const std::string& xpath, gnmi::Path* path)
{
  if (xpath.empty() || !path) return;

  size_t pos = (xpath[0] == '/') ? 1 : 0;

  while (pos < xpath.size()) {
    size_t next = xpath.find('/', pos);
    if (next == std::string::npos) next = xpath.size();

    std::string part = xpath.substr(pos, next - pos);
    if (part.empty()) { pos = next + 1; continue; }

    auto* elem = path->add_elem();
    size_t bracket = part.find('[');

    if (bracket == std::string::npos) {
      elem->set_name(part);
    } else {
      elem->set_name(part.substr(0, bracket));
      // parse all [key=value] predicates in this element
      size_t k = bracket;
      while (k < part.size() && part[k] == '[') {
        size_t eq  = part.find('=', k + 1);
        size_t end = part.find(']', k + 1);
        if (eq == std::string::npos || end == std::string::npos) break;

        std::string key = part.substr(k + 1, eq - k - 1);
        std::string val = part.substr(eq + 1, end - eq - 1);
        // strip surrounding quotes if present
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\''))
          val = val.substr(1, val.size() - 2);

        (*elem->mutable_key())[key] = val;
        k = end + 1;
      }
    }

    pos = next + 1;
  }
}

// Remove the double quotes gnmi_to_xpath puts around key values, so that
// [name="PSC-0"] and [name=PSC-0] compare equal.
inline std::string stripPathQuotes(const std::string& xpath) {
    std::string out;
    out.reserve(xpath.size());
    for (char c : xpath)
        if (c != '"') out += c;
    return out;
}

// True if `prefix` is a path-prefix of `path` ending at a segment boundary:
// /foo matches /foo, /foo/bar, /foo[k=v] but NOT /foobar. Both arguments must
// already be quote-normalised (see stripPathQuotes).
inline bool isPathPrefix(const std::string& prefix, const std::string& path) {
    if (!path.starts_with(prefix)) return false;
    if (path.size() == prefix.size()) return true;
    const char next = path[prefix.size()];
    return next == '/' || next == '[';
}

// Validate a gNMI Path.origin at the protocol boundary (D16 single-origin core,
// backlog C1). The core only knows origin-less paths, so the boundary owns
// origin policy:
//   - empty       → the default schema ("openconfig"); accepted, nothing to do.
//   - "openconfig" → the one implemented schema; accepted.
//   - any other    → a syntactically valid origin naming a schema we do not
//                     implement → UNIMPLEMENTED (spec §3.3.4 L1152 / §3.5.2.4
//                     L1900, finding N). INVALID_ARGUMENT is reserved for a
//                     malformed Path (validateGnmiPath), checked separately.
// Origin is stripped for free downstream: gnmi_to_xpath no longer embeds it.
inline grpc::Status validateOrigin(const std::string& origin) {
    if (origin.empty() || origin == "openconfig")
        return grpc::Status::OK;
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "unsupported origin (only \"openconfig\" is implemented): "
                            + origin);
}

// Returns INVALID_ARGUMENT if the Path has structural issues:
// empty element name or empty key name.
// An empty path (no elems) is valid — it represents a root query.
inline grpc::Status validateGnmiPath(const gnmi::Path& path) {
    for (const auto& elem : path.elem()) {
        if (elem.name().empty())
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "path element has empty name");
        for (const auto& kv : elem.key()) {
            if (kv.first.empty())
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                    "path element key has empty name");
        }
    }
    return grpc::Status::OK;
}

#endif // _UTILS_H
