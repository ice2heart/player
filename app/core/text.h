#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace oap {

// "1 hour 4 minutes", "3 minutes 20 seconds", "45 seconds".
std::string spokenDuration(double seconds);

// An ISO 8601 time such as "2026-10-20T18:01:44.120Z" (fraction and zone optional; a zone
// offset is applied) as seconds since 1970, or 0 when `text` is not one. Pure arithmetic: no
// time zone database, no clock.
int64_t parseIsoTime(const std::string& text);

// Decodes the UTF-8 code point at s[i] and moves i past it. A malformed byte stands for itself.
char32_t nextCodePoint(std::string_view s, size_t& i);

// Appends the UTF-8 form of a code point.
void appendUtf8(std::string& out, char32_t c);

// Lower case for what books and artists are named in: ASCII, Latin-1, Latin Extended-A
// (Polish, Czech, Turkish...), Greek and Cyrillic. ё and Ё fold to е, and the final ς to σ.
// Accents stay ("É" is "é"). Other scripts are returned as they are.
char32_t foldCase(char32_t c);

} // namespace oap
