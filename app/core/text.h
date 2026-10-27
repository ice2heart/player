#pragma once

#include <cstdint>
#include <string>

namespace oap {

// "1 hour 4 minutes", "3 minutes 20 seconds", "45 seconds".
std::string spokenDuration(double seconds);

// An ISO 8601 time such as "2026-10-20T18:01:44.120Z" (fraction and zone optional; a zone
// offset is applied) as seconds since 1970, or 0 when `text` is not one. Pure arithmetic: no
// time zone database, no clock.
int64_t parseIsoTime(const std::string& text);

} // namespace oap
