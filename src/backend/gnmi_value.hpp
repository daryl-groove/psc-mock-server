/*
 * gnmi_value — the single scalar -> gnmi::TypedValue authority.
 *
 * Everywhere a C++ scalar is wrapped as a leaf value (a provider's declared
 * leaves, the sensor walk, a test) routes through here, so the union-field
 * mapping lives in exactly one place. Covers the gNMI scalar types used in the
 * OpenConfig models this mock serves.
 */

#pragma once

#include <cstdint>
#include <string>

#include "gnmi.pb.h"

namespace gnmid {

inline gnmi::TypedValue typedValue(double v)   { gnmi::TypedValue t; t.set_double_val(v); return t; }
inline gnmi::TypedValue typedValue(bool v)     { gnmi::TypedValue t; t.set_bool_val(v);   return t; }
inline gnmi::TypedValue typedValue(int64_t v)  { gnmi::TypedValue t; t.set_int_val(v);    return t; }
inline gnmi::TypedValue typedValue(uint64_t v) { gnmi::TypedValue t; t.set_uint_val(v);   return t; }
inline gnmi::TypedValue typedValue(const std::string& v) {
    gnmi::TypedValue t; t.set_string_val(v); return t;
}
// A bare const char* would otherwise bind to typedValue(bool) — route it to string.
inline gnmi::TypedValue typedValue(const char* v) { return typedValue(std::string(v)); }

}  // namespace gnmid
