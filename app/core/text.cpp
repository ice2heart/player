#include "text.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace oap {

char32_t nextCodePoint(std::string_view s, size_t& i) {
    unsigned char c = s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len <= 1 || i + len > s.size()) {
        ++i;
        return c;
    }
    char32_t cp = c & (0x7F >> len);
    for (int k = 1; k < len; ++k) {
        unsigned char cc = s[i + k];
        if ((cc & 0xC0) != 0x80) {
            ++i;
            return c;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += len;
    return cp;
}

void appendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

char32_t foldCase(char32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 0x20;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20; // À..Þ
    if ((c >= 0x100 && c <= 0x137 && c != 0x130 && c != 0x131) || (c >= 0x14A && c <= 0x177)) return c % 2 == 0 ? c + 1 : c; // Latin Extended-A
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c % 2 == 1 ? c + 1 : c;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 0x20; // Greek
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC; // Ό
    if (c == 0x38E || c == 0x38F) return c + 63; // Ύ Ώ
    if (c == 0x3C2) return 0x3C3; // final ς
    if (c == 0x401 || c == 0x451) return 0x435; // Ё ё are е
    if (c >= 0x410 && c <= 0x42F) return c + 0x20; // А..Я
    if (c >= 0x400 && c <= 0x40F) return c + 0x50; // Ѐ..Џ
    return c;
}

std::string spokenDuration(double seconds) {
    long total = std::lround(std::max(0.0, seconds));
    long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    auto unit = [](long n, const char* name) {
        return std::to_string(n) + " " + name + (n == 1 ? "" : "s");
    };
    std::vector<std::string> parts;
    if (h) parts.push_back(unit(h, "hour"));
    if (m) parts.push_back(unit(m, "minute"));
    if (!h && (s || parts.empty())) parts.push_back(unit(s, "second")); // seconds only matter under an hour
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : " ") + p;
    return out;
}

namespace {
// Days from 1970-01-01 to a civil date (proleptic Gregorian).
int64_t daysFromCivil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// `n` digits at text[i], advancing i; -1 if they are not all digits.
int digits(const std::string& text, size_t& i, size_t n) {
    if (i + n > text.size()) return -1;
    int v = 0;
    for (size_t k = 0; k < n; ++k) {
        char c = text[i + k];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    i += n;
    return v;
}

bool take(const std::string& text, size_t& i, char c) {
    if (i < text.size() && text[i] == c) {
        ++i;
        return true;
    }
    return false;
}
} // namespace

int64_t parseIsoTime(const std::string& text) {
    size_t i = 0;
    int year = digits(text, i, 4);
    if (year < 0 || !take(text, i, '-')) return 0;
    int month = digits(text, i, 2);
    if (month < 1 || month > 12 || !take(text, i, '-')) return 0;
    int day = digits(text, i, 2);
    if (day < 1 || day > 31) return 0;
    int hour = 0, minute = 0, second = 0;
    if (take(text, i, 'T') || take(text, i, ' ')) {
        hour = digits(text, i, 2);
        if (hour < 0 || hour > 23 || !take(text, i, ':')) return 0;
        minute = digits(text, i, 2);
        if (minute < 0 || minute > 59 || !take(text, i, ':')) return 0;
        second = digits(text, i, 2);
        if (second < 0 || second > 60) return 0;
        if (take(text, i, '.'))
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    }
    int64_t offset = 0; // seconds the written time is ahead of UTC
    if (i < text.size()) {
        if (take(text, i, 'Z')) {
        } else if (text[i] == '+' || text[i] == '-') {
            int sign = text[i++] == '-' ? -1 : 1;
            int oh = digits(text, i, 2);
            take(text, i, ':');
            int om = i == text.size() ? 0 : digits(text, i, 2);
            if (oh < 0 || om < 0) return 0;
            offset = sign * (oh * 3600 + om * 60);
        }
        if (i != text.size()) return 0;
    }
    return daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second - offset;
}

namespace {

// Unicode blocks the script of a letter is read from.
constexpr char32_t kLatin1Letters = 0xC0, kMultiply = 0xD7, kDivide = 0xF7, kLatinExtBEnd = 0x24F;
constexpr char32_t kLatinExtAdditional = 0x1E00, kLatinExtAdditionalEnd = 0x1EFF;
constexpr char32_t kGreekStart = 0x370, kGreekEnd = 0x3FF, kGreekExtStart = 0x1F00, kGreekExtEnd = 0x1FFF;
constexpr char32_t kCyrillicStart = 0x400, kCyrillicEnd = 0x52F, kCyrillicSupStart = 0x1C80,
                   kCyrillicSupEnd = 0x1C8F, kCyrillicExtBStart = 0xA640, kCyrillicExtBEnd = 0xA69F;
constexpr char32_t kHebrewStart = 0x590, kHebrewEnd = 0x5FF, kHebrewPresStart = 0xFB1D, kHebrewPresEnd = 0xFB4F;
constexpr char32_t kArabicStart = 0x600, kArabicEnd = 0x6FF, kArabicSupStart = 0x750, kArabicSupEnd = 0x77F,
                   kArabicPresAStart = 0xFB50, kArabicPresAEnd = 0xFDFF, kArabicPresBStart = 0xFE70,
                   kArabicPresBEnd = 0xFEFF;
constexpr char32_t kHiraganaStart = 0x3040, kKatakanaEnd = 0x30FF, kKatakanaExtStart = 0x31F0,
                   kKatakanaExtEnd = 0x31FF, kHalfwidthKatakanaStart = 0xFF66, kHalfwidthKatakanaEnd = 0xFF9F;
constexpr char32_t kHangulJamoStart = 0x1100, kHangulJamoEnd = 0x11FF, kHangulCompatStart = 0x3130,
                   kHangulCompatEnd = 0x318F, kHangulSyllablesStart = 0xAC00, kHangulSyllablesEnd = 0xD7AF;
constexpr char32_t kHanExtAStart = 0x3400, kHanExtAEnd = 0x4DBF, kHanStart = 0x4E00, kHanEnd = 0x9FFF,
                   kHanCompatStart = 0xF900, kHanCompatEnd = 0xFAFF, kHanExtBStart = 0x20000,
                   kHanExtEnd = 0x323AF;
constexpr char32_t kKatakanaMiddleDot = 0x30FB;
constexpr char32_t kGeneralPunctuationStart = 0x2000, kGeneralPunctuationEnd = 0x206F;
constexpr char32_t kCjkSymbolsStart = 0x3000, kCjkSymbolsEnd = 0x303F;
constexpr char32_t kFullwidthFormsStart = 0xFF00, kFullwidthFormsEnd = 0xFF65; // fullwidth ASCII, punctuation
constexpr char32_t kSymbolsStart = 0x2070, kSymbolsEnd = 0x2BFF; // super/subscripts, currency, arrows, math, shapes
constexpr char32_t kEmojiStart = 0x1F000, kEmojiEnd = 0x1FAFF;
constexpr char32_t kLatinLigaturesStart = 0xFB00, kLatinLigaturesEnd = 0xFB06;
constexpr char32_t kCombiningMarksStart = 0x300, kCombiningMarksEnd = 0x36F, kModifierLettersStart = 0x2B0,
                   kModifierLettersEnd = 0x2FF, kVariationSelectorsStart = 0xFE00, kVariationSelectorsEnd = 0xFE0F,
                   kVariationSelectorsSupStart = 0xE0100, kVariationSelectorsSupEnd = 0xE01EF;
constexpr char32_t kIterationMark = 0x3005, kClosingMark = 0x3007; // 々 〆 〇 are Han letters
constexpr char32_t kEnclosedCjkStart = 0x3190, kEnclosedCjkEnd = 0x33FF, kCjkFormsStart = 0xFE30,
                   kCjkFormsEnd = 0xFE6F, kFullwidthSignsStart = 0xFFE0, kFullwidthSignsEnd = 0xFFEE;
constexpr char32_t kSurrogatesStart = 0xD800, kSurrogatesEnd = 0xDFFF, kMaxCodePoint = 0x10FFFF;
constexpr char32_t kArabicIndicDigit0 = 0x660, kArabicIndicDigit9 = 0x669;

bool in(char32_t c, char32_t a, char32_t b) { return c >= a && c <= b; }

// A mark belongs to the letter before it.
enum class Kind { Neutral, Letter, Mark };

struct Cls {
    Kind kind;
    Script script;
};

Cls classify(char32_t c, bool malformed) {
    const Cls neutral{Kind::Neutral, Script::Latin};
    auto letter = [](Script s) { return Cls{Kind::Letter, s}; };
    if (malformed || in(c, kSurrogatesStart, kSurrogatesEnd) || c > kMaxCodePoint) return neutral;
    if (c < 0x80) {
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        return alpha ? letter(Script::Latin) : neutral;
    }
    if (c < kLatin1Letters) return neutral;
    if (in(c, kCombiningMarksStart, kCombiningMarksEnd) || in(c, kModifierLettersStart, kModifierLettersEnd) ||
        in(c, kVariationSelectorsStart, kVariationSelectorsEnd) ||
        in(c, kVariationSelectorsSupStart, kVariationSelectorsSupEnd))
        return {Kind::Mark, Script::Latin};
    if (c <= kLatinExtBEnd) return (c == kMultiply || c == kDivide) ? neutral : letter(Script::Latin);
    if (in(c, kLatinExtAdditional, kLatinExtAdditionalEnd) || in(c, kLatinLigaturesStart, kLatinLigaturesEnd)) return letter(Script::Latin);
    if (in(c, kGreekStart, kGreekEnd) || in(c, kGreekExtStart, kGreekExtEnd)) return letter(Script::Greek);
    if (in(c, kCyrillicStart, kCyrillicEnd) || in(c, kCyrillicSupStart, kCyrillicSupEnd) ||
        in(c, kCyrillicExtBStart, kCyrillicExtBEnd))
        return letter(Script::Cyrillic);
    if (in(c, kHebrewStart, kHebrewEnd) || in(c, kHebrewPresStart, kHebrewPresEnd)) return letter(Script::Hebrew);
    if (in(c, kArabicIndicDigit0, kArabicIndicDigit9)) return neutral;
    if (in(c, kArabicStart, kArabicEnd) || in(c, kArabicSupStart, kArabicSupEnd) ||
        in(c, kArabicPresAStart, kArabicPresAEnd) || in(c, kArabicPresBStart, kArabicPresBEnd))
        return letter(Script::Arabic);
    if (in(c, kIterationMark, kClosingMark)) return letter(Script::Han);
    if (in(c, kGeneralPunctuationStart, kGeneralPunctuationEnd) || in(c, kCjkSymbolsStart, kCjkSymbolsEnd) ||
        in(c, kSymbolsStart, kSymbolsEnd) || in(c, kFullwidthFormsStart, kFullwidthFormsEnd) ||
        in(c, kEmojiStart, kEmojiEnd) ||
        in(c, kEnclosedCjkStart, kEnclosedCjkEnd) || in(c, kCjkFormsStart, kCjkFormsEnd) ||
        in(c, kFullwidthSignsStart, kFullwidthSignsEnd))
        return neutral;
    if (c == kKatakanaMiddleDot) return neutral;
    if (in(c, kHiraganaStart, kKatakanaEnd) || in(c, kKatakanaExtStart, kKatakanaExtEnd) ||
        in(c, kHalfwidthKatakanaStart, kHalfwidthKatakanaEnd))
        return letter(Script::Japanese);
    if (in(c, kHangulJamoStart, kHangulJamoEnd) || in(c, kHangulCompatStart, kHangulCompatEnd) ||
        in(c, kHangulSyllablesStart, kHangulSyllablesEnd))
        return letter(Script::Hangul);
    if (in(c, kHanExtAStart, kHanExtAEnd) || in(c, kHanStart, kHanEnd) || in(c, kHanCompatStart, kHanCompatEnd) ||
        in(c, kHanExtBStart, kHanExtEnd))
        return letter(Script::Han);
    return letter(Script::Other);
}

bool japanese(Script s) { return s == Script::Han || s == Script::Japanese; }

} // namespace

std::vector<ScriptRun> scriptRuns(std::string_view text) {
    struct Piece {
        size_t begin, end;
        Cls cls;
    };
    std::vector<Piece> pieces;
    for (size_t i = 0; i < text.size();) {
        size_t begin = i;
        char32_t c = nextCodePoint(text, i);
        bool malformed = static_cast<unsigned char>(text[begin]) >= 0x80 && i - begin == 1;
        pieces.push_back({begin, i, classify(c, malformed)});
    }
    for (size_t k = 0; k < pieces.size(); ++k)
        if (pieces[k].cls.kind == Kind::Mark)
            pieces[k].cls = k > 0 && pieces[k - 1].cls.kind == Kind::Letter ? pieces[k - 1].cls
                                                                              : Cls{Kind::Neutral, Script::Latin};
    // Marks join the letter before them. A stretch of neutrals takes the script of the letters on
    // both sides when they agree (Han and kana agree as Japanese), else Latin.
    std::vector<Script> scripts(pieces.size(), Script::Latin);
    for (size_t k = 0; k < pieces.size();) {
        if (pieces[k].cls.kind == Kind::Letter) {
            scripts[k] = pieces[k].cls.script;
            ++k;
            continue;
        }
        size_t e = k;
        while (e < pieces.size() && pieces[e].cls.kind == Kind::Neutral) ++e;
        if (k > 0 && e < pieces.size()) {
            Script before = pieces[k - 1].cls.script, after = pieces[e].cls.script;
            if (before == after || (japanese(before) && japanese(after)))
                for (size_t j = k; j < e; ++j) scripts[j] = before; // a Han/kana mix is promoted below
        }
        k = e;
    }
    std::vector<ScriptRun> runs;
    for (size_t k = 0; k < pieces.size(); ++k) {
        Script s = scripts[k];
        std::string_view bytes = text.substr(pieces[k].begin, pieces[k].end - pieces[k].begin);
        if (!runs.empty() && (runs.back().script == s || (japanese(runs.back().script) && japanese(s)))) {
            if (runs.back().script != s) runs.back().script = Script::Japanese;
            runs.back().text.append(bytes);
        } else {
            runs.push_back({s, std::string(bytes)});
        }
    }
    return runs;
}

} // namespace oap
