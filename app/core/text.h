#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

enum class Script { Latin, Cyrillic, Greek, Han, Japanese, Hangul, Hebrew, Arabic, Other };

struct ScriptRun {
    Script script;
    std::string text; // UTF-8, the bytes of the input
};

// Cuts `text` into runs of one script, in order; the texts joined are `text`. Kana is labelled
// Japanese (a run of kana alone is Japanese too), and Han directly next to kana joins them
// into one Japanese run; Han with no kana is Han (Chinese). Digits, spaces and punctuation have
// no script: between two letters of the same script they stay in that run, everywhere else
// (between two scripts, at either end) they join a Latin run. A combining mark or variation selector
// stays with the letter before it. A malformed UTF-8 byte counts as punctuation. Letters of any script not listed are Other.
std::vector<ScriptRun> scriptRuns(std::string_view text);

// Plan B: the run in Latin letters, for the English voice to read. Cyrillic is a practical
// romanisation (BGN/PCGN without diacritics: "Жуков" is "Zhukov"), Greek ELOT 743 without
// accents, kana Hepburn without macrons (a long vowel is written as the kana write it; っ doubles
// the next consonant, "ch" becoming "tch"; ー doubles the vowel before it), Hangul Revised
// Romanization of each syllable on its own (no sound changes between syllables). A letter that
// has a capital keeps it: the first letter of a word, "Zh" for Ж, "ZH" in an all-capitals word.
// Spaces, digits and punctuation pass through unchanged; combining marks and letters of the script
// that have no entry (half-width kana, lone jamo, ъ and ь) give nothing. A Latin run is returned
// as it is. Han, Hebrew, Arabic and Other give an empty string: the caller names the script. So
// does a Japanese run with any kanji in it (there is no reading table): the caller speaks that run
// as "Japanese text" rather than a half-spoken name.
std::string transliterate(const ScriptRun& run);

} // namespace oap
