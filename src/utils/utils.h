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

using std::chrono::system_clock;
using std::chrono::duration_cast;
using std::chrono::nanoseconds;

/* Get current time since epoch in nanosec */
inline uint64_t get_time_nanosec()
{
  nanoseconds ts;
  ts = duration_cast<nanoseconds>(system_clock::now().time_since_epoch());
  return ts.count();
}

/* gNMI Path → xpath string
 * e.g. {elem:[{name:"components"},{name:"component",key:{name:"PSC-0"}}]}
 *   →  "/components/component[name=\"PSC-0\"]"
 */
inline std::string gnmi_to_xpath(const gnmi::Path& path)
{
  std::string str;

  if (path.elem_size() <= 0)
    return str;

  bool first = true;
  for (auto& node : path.elem()) {
    str += "/";
    if (first) {
      first = false;
      if (!path.origin().empty())
        str += path.origin() + ":";
    }
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

#endif // _UTILS_H
