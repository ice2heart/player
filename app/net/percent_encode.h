#pragma once

#include <string>

namespace oap {

// RFC 3986: everything but the unreserved characters is %XX-encoded, so ids,
// names and passwords with '&', '=', '/', spaces or UTF-8 survive.
std::string percentEncode(const std::string& s);

} // namespace oap
