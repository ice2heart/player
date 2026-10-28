#include "text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>
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
    if (c == 0x178) return 0xFF; // Ÿ
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 0x20; // Greek, with Ϊ Ϋ
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

// A stray byte, or a sequence that is not the shortest form of a code point, a surrogate or past U+10FFFF.
bool isMalformed(std::string_view s, size_t begin, size_t end) {
    size_t len = end - begin;
    if (static_cast<unsigned char>(s[begin]) < 0x80) return false;
    if (len == 1) return true;
    size_t i = begin;
    char32_t c = nextCodePoint(s.substr(0, end), i);
    constexpr char32_t kShortest[] = {0, 0, 0x80, 0x800, 0x10000};
    return c < kShortest[len] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF);
}

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
        bool malformed = isMalformed(text, begin, i);
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

namespace {

constexpr char32_t kCyrillicGhe = 0x490, kCyrillicGheLower = 0x491, kCyrillicYo = 0x401, kCyrillicYoLower = 0x451;
constexpr char32_t kGreekFinalSigma = 0x3C2, kGreekSigma = 0x3C3, kGreekDialytikaIota = 0x3AA,
                   kGreekDialytikaUpsilon = 0x3AB;
constexpr char32_t kHiraganaFirst = 0x3041, kHiraganaLast = 0x3094, kKatakanaFirst = 0x30A1, kKatakanaLast = 0x30F4,
                   kKatakanaToHiragana = 0x60, kProlongedSoundMark = 0x30FC;
constexpr char32_t kSmallA = 0x3041, kSmallI = 0x3043, kSmallU = 0x3045, kSmallE = 0x3047, kSmallO = 0x3049,
                   kSmallYa = 0x3083, kSmallYu = 0x3085, kSmallYo = 0x3087, kSmallTsu = 0x3063,
                   kHiraganaU = 0x3046, kHiraganaFu = 0x3075, kHiraganaVu = 0x3094;
constexpr char32_t kHangulSyllableFirst = 0xAC00, kHangulSyllableLast = 0xD7A3;
constexpr int kHangulMedials = 21, kHangulFinals = 28;

struct Item {
    size_t begin, end; // bytes of the input
    char32_t c;
    Cls cls;
};

// Neutral characters and Latin letters are copied; a combining mark is not.
bool copied(const Item& it) { return it.cls.kind == Kind::Neutral || (it.cls.kind == Kind::Letter && it.cls.script == Script::Latin); }

bool isVowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

// A letter with a capital: the first letter of a word and a word in capitals keep the case.
void appendCased(std::string& out, const std::string& latin, bool upper, bool allCaps) {
    for (size_t k = 0; k < latin.size(); ++k)
        out += (upper && (allCaps || k == 0)) ? static_cast<char>(latin[k] - 'a' + 'A') : latin[k];
}

// Is a capital letter next to items first..last, or one of them after the first? Then the word
// is in capitals. `upper[k]` is true for the items that are capital letters.
bool capitalsNear(const std::vector<bool>& upper, size_t first, size_t last) {
    for (size_t k = first + 1; k <= last; ++k)
        if (upper[k]) return true;
    return (first > 0 && upper[first - 1]) || (last + 1 < upper.size() && upper[last + 1]);
}

char32_t lowerCyrillic(char32_t c) {
    if (c == kCyrillicYo || c == kCyrillicYoLower) return kCyrillicYoLower; // foldCase would make it е
    if (c == kCyrillicGhe) return kCyrillicGheLower;
    return foldCase(c);
}

const char* cyrillic(char32_t c) {
    switch (c) {
    case 0x430: return "a";   case 0x431: return "b";   case 0x432: return "v";   case 0x433: return "g";
    case 0x434: return "d";   case 0x435: return "e";   case 0x436: return "zh";  case 0x437: return "z";
    case 0x438: return "i";   case 0x439: return "y";   case 0x43A: return "k";   case 0x43B: return "l";
    case 0x43C: return "m";   case 0x43D: return "n";   case 0x43E: return "o";   case 0x43F: return "p";
    case 0x440: return "r";   case 0x441: return "s";   case 0x442: return "t";   case 0x443: return "u";
    case 0x444: return "f";   case 0x445: return "kh";  case 0x446: return "ts";  case 0x447: return "ch";
    case 0x448: return "sh";  case 0x449: return "shch"; case 0x44A: return "";    case 0x44B: return "y";
    case 0x44C: return "";    case 0x44D: return "e";   case 0x44E: return "yu";  case 0x44F: return "ya";
    case kCyrillicYoLower: return "yo";
    case 0x452: return "dj";  // ђ
    case 0x453: return "g";   // ѓ
    case 0x454: return "ye";  // є
    case 0x455: return "dz";  // ѕ
    case 0x456: return "i";   // і
    case 0x457: return "yi";  // ї
    case 0x458: return "j";   // ј
    case 0x459: return "lj";  // љ
    case 0x45A: return "nj";  // њ
    case 0x45B: return "ch";  // ћ
    case 0x45C: return "k";   // ќ
    case 0x45E: return "u";   // ў
    case 0x45F: return "dzh"; // џ
    case kCyrillicGheLower: return "g"; // ґ
    default: return nullptr;
    }
}

void transliterateCyrillic(const std::vector<Item>& items, std::string_view text, std::string& out) {
    std::vector<bool> upper(items.size(), false);
    for (size_t k = 0; k < items.size(); ++k)
        upper[k] = items[k].cls.kind == Kind::Letter && items[k].cls.script == Script::Cyrillic &&
                   lowerCyrillic(items[k].c) != items[k].c;
    for (size_t k = 0; k < items.size(); ++k) {
        const Item& it = items[k];
        if (it.cls.kind != Kind::Letter || it.cls.script != Script::Cyrillic) {
            if (copied(it))
                out.append(text.substr(it.begin, it.end - it.begin));
            continue;
        }
        if (const char* latin = cyrillic(lowerCyrillic(it.c))) appendCased(out, latin, upper[k], capitalsNear(upper, k, k));
    }
}

// The lower case Greek letter with its accent taken off.
char32_t greekBase(char32_t c) {
    if (c == kGreekFinalSigma) return kGreekSigma;
    if (c == kGreekDialytikaIota) c = 0x3CA;
    if (c == kGreekDialytikaUpsilon) c = 0x3CB;
    c = foldCase(c);
    switch (c) {
    case 0x3AC: return 0x3B1; case 0x3AD: return 0x3B5; case 0x3AE: return 0x3B7; case 0x3AF: return 0x3B9;
    case 0x3CA: return 0x3B9; case 0x390: return 0x3B9;  case 0x3CC: return 0x3BF; case 0x3CD: return 0x3C5;
    case 0x3CB: return 0x3C5; case 0x3B0: return 0x3C5;  case 0x3CE: return 0x3C9;
    default: return c;
    }
}

const char* greek(char32_t c) {
    switch (c) {
    case 0x3B1: return "a";  case 0x3B2: return "v";  case 0x3B3: return "g";  case 0x3B4: return "d";
    case 0x3B5: return "e";  case 0x3B6: return "z";  case 0x3B7: return "i";  case 0x3B8: return "th";
    case 0x3B9: return "i";  case 0x3BA: return "k";  case 0x3BB: return "l";  case 0x3BC: return "m";
    case 0x3BD: return "n";  case 0x3BE: return "x";  case 0x3BF: return "o";  case 0x3C0: return "p";
    case 0x3C1: return "r";  case kGreekSigma: return "s"; case 0x3C4: return "t"; case 0x3C5: return "y";
    case 0x3C6: return "f";  case 0x3C7: return "ch"; case 0x3C8: return "ps"; case 0x3C9: return "o";
    default: return nullptr;
    }
}

bool upsilonWithDiaeresis(char32_t c) { // ϋ ΰ Ϋ: the vowel before it is read on its own (αϋ is not αυ)
    return c == 0x3CB || c == 0x3B0 || c == kGreekDialytikaUpsilon;
}

// "av" before a vowel or a voiced consonant, "af" before a voiceless one or at the end.
bool greekVoiced(char32_t next) {
    switch (next) {
    case 0x3B1: case 0x3B2: case 0x3B3: case 0x3B4: case 0x3B5: case 0x3B6: case 0x3B7: case 0x3B9: case 0x3BB:
    case 0x3BC: case 0x3BD: case 0x3BF: case 0x3C1: case 0x3C5: case 0x3C9: return true;
    default: return false;
    }
}

void transliterateGreek(const std::vector<Item>& items, std::string_view text, std::string& out) {
    auto isGreek = [&](size_t k) { return k < items.size() && items[k].cls.kind == Kind::Letter && items[k].cls.script == Script::Greek; };
    std::vector<bool> upper(items.size(), false);
    std::vector<char32_t> base(items.size(), 0);
    for (size_t k = 0; k < items.size(); ++k) {
        if (!isGreek(k)) continue;
        char32_t c = items[k].c;
        upper[k] = (c != kGreekFinalSigma && foldCase(c) != c) || c == kGreekDialytikaIota || c == kGreekDialytikaUpsilon;
        base[k] = greekBase(c);
    }
    auto baseAt = [&](size_t k) { return isGreek(k) ? base[k] : char32_t(0); };
    for (size_t k = 0; k < items.size(); ++k) {
        const Item& it = items[k];
        if (!isGreek(k)) {
            if (copied(it))
                out.append(text.substr(it.begin, it.end - it.begin));
            continue;
        }
        bool wordStart = k == 0 || !isGreek(k - 1);
        char32_t b = base[k], n = baseAt(k + 1);
        if (isGreek(k + 1) && upsilonWithDiaeresis(items[k + 1].c)) n = 0;
        std::string latin;
        size_t used = 1;
        if (b == 0x3BF && n == 0x3C5) { latin = "ou"; used = 2; }
        else if ((b == 0x3B1 || b == 0x3B5 || b == 0x3B7) && n == 0x3C5) {
            latin = std::string(1, b == 0x3B1 ? 'a' : b == 0x3B5 ? 'e' : 'i') + (greekVoiced(baseAt(k + 2)) ? 'v' : 'f');
            used = 2;
        } else if (b == 0x3B3 && n == 0x3B3) { latin = "ng"; used = 2; }
        else if (b == 0x3B3 && n == 0x3BA) { latin = wordStart ? "g" : "ng"; used = 2; }
        else if (b == 0x3B3 && n == 0x3BE) { latin = "nx"; used = 2; }
        else if (b == 0x3B3 && n == 0x3C7) { latin = "nch"; used = 2; }
        else if (b == 0x3BC && n == 0x3C0) { latin = wordStart ? "b" : "mp"; used = 2; }
        else if (b == 0x3BD && n == 0x3C4) { latin = wordStart ? "d" : "nt"; used = 2; }
        else if (const char* single = greek(b)) latin = single;
        appendCased(out, latin, upper[k], capitalsNear(upper, k, k + used - 1));
        k += used - 1;
    }
}

const char* const kHiragana[] = { // U+3041 ぁ .. U+3094 ゔ
    "a", "a", "i", "i", "u", "u", "e", "e", "o", "o",                                        // ぁ あ ぃ い ぅ う ぇ え ぉ お
    "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go",                                // か..
    "sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo",                              // さ..
    "ta", "da", "chi", "ji", "tsu", "tsu", "zu", "te", "de", "to", "do",                       // た だ ち ぢ っ つ づ て で と ど (っ is handled apart)
    "na", "ni", "nu", "ne", "no",                                                              // な..
    "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po", // は..
    "ma", "mi", "mu", "me", "mo",                                                              // ま..
    "ya", "ya", "yu", "yu", "yo", "yo",                                                        // ゃ や ゅ ゆ ょ よ
    "ra", "ri", "ru", "re", "ro",                                                              // ら..
    "wa", "wa", "i", "e", "o", "n",                                                            // ゎ わ ゐ ゑ を ん
    "vu"};                                                                                      // ゔ
static_assert(sizeof(kHiragana) / sizeof(kHiragana[0]) == kHiraganaVu - kHiraganaFirst + 1, "one entry per kana");

bool kanaLetter(const Item& it) { return it.cls.kind == Kind::Letter && it.cls.script == Script::Japanese; }

// The hiragana a kana stands for (katakana is moved over), or 0 for what is not a kana with a reading.
char32_t hiraganaOf(const Item& it) {
    if (!kanaLetter(it)) return 0;
    char32_t c = it.c;
    if (c >= kKatakanaFirst && c <= kKatakanaLast) c -= kKatakanaToHiragana;
    return c >= kHiraganaFirst && c <= kHiraganaLast ? c : 0;
}

// Katakana for sounds Japanese had no kana for: ティ ディ トゥ ドゥ テュ デュ ツァ ツィ ツェ ツォ.
struct Loan {
    char32_t base, small;
    const char* latin;
};
const Loan kLoans[] = {{U'て', kSmallI, "ti"},   {U'で', kSmallI, "di"},   {U'と', kSmallU, "tu"},
                       {U'ど', kSmallU, "du"},   {U'て', kSmallYu, "tyu"}, {U'で', kSmallYu, "dyu"},
                       {U'つ', kSmallA, "tsa"},  {U'つ', kSmallI, "tsi"},  {U'つ', kSmallE, "tse"},
                       {U'つ', kSmallO, "tso"}};

bool isIRow(char32_t h) { // き ぎ し じ ち ぢ に ひ び ぴ み り
    for (char32_t row : {U'き', U'ぎ', U'し', U'じ', U'ち', U'ぢ', U'に', U'ひ', U'び', U'ぴ', U'み', U'り'})
        if (h == row) return true;
    return false;
}

void transliterateJapanese(const std::vector<Item>& items, std::string_view text, std::string& out) {
    bool doubleNext = false;
    char lastVowel = 0;
    auto emit = [&](const std::string& s) {
        if (doubleNext && !s.empty() && !isVowel(s[0])) out += s.compare(0, 2, "ch") == 0 ? 't' : s[0];
        doubleNext = false;
        out += s;
        lastVowel = isVowel(s.back()) ? s.back() : 0;
    };
    for (size_t k = 0; k < items.size(); ++k) {
        const Item& it = items[k];
        if (!kanaLetter(it)) {
            if (copied(it)) {
                out.append(text.substr(it.begin, it.end - it.begin));
                lastVowel = 0;
                doubleNext = false; // a っ at the end of a word doubles nothing
            }
            continue;
        }
        if (it.c == kProlongedSoundMark) {
            if (lastVowel) out += lastVowel;
            continue;
        }
        char32_t h = hiraganaOf(it);
        if (h == 0) continue;
        if (h == kSmallTsu) {
            doubleNext = true;
            continue;
        }
        char32_t next = k + 1 < items.size() ? hiraganaOf(items[k + 1]) : 0;
        std::string base = kHiragana[h - kHiraganaFirst];
        const Loan* loan = nullptr;
        for (const Loan& l : kLoans)
            if (l.base == h && l.small == next) loan = &l;
        if (loan) {
            emit(loan->latin);
            ++k;
            continue;
        }
        if (isIRow(h) && (next == kSmallYa || next == kSmallYu || next == kSmallYo || next == kSmallE)) {
            std::string stem = base.substr(0, base.size() - 1); // "ki" -> "k"
            bool palatal = stem == "sh" || stem == "ch" || stem == "j"; // no y of their own
            std::string tail = next == kSmallYa ? "a" : next == kSmallYu ? "u" : next == kSmallYo ? "o" : "e";
            emit(stem + (palatal ? "" : "y") + tail);
            ++k;
        } else if ((h == kHiraganaU || h == kHiraganaFu || h == kHiraganaVu) &&
                   (next == kSmallA || next == kSmallI || next == kSmallE || next == kSmallO)) {
            std::string stem = h == kHiraganaU ? "w" : h == kHiraganaFu ? "f" : "v";
            emit(stem + (next == kSmallA ? "a" : next == kSmallI ? "i" : next == kSmallE ? "e" : "o"));
            ++k;
        } else {
            emit(base);
        }
    }
}

void transliterateHangul(const std::vector<Item>& items, std::string_view text, std::string& out) {
    static const char* const initials[] = {"g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s",
                                           "ss", "", "j", "jj", "ch", "k", "t", "p", "h"};
    static const char* const medials[] = {"a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae",
                                          "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i"};
    static const char* const finals[] = {"", "k", "k", "k", "n", "n", "n", "t", "l", "k", "m", "l", "l",
                                         "l", "p", "l", "m", "p", "p", "t", "t", "ng", "t", "t", "k", "t", "p", "t"};
    for (const Item& it : items) {
        if (copied(it)) {
            out.append(text.substr(it.begin, it.end - it.begin));
        } else if (it.cls.kind == Kind::Letter && it.c >= kHangulSyllableFirst && it.c <= kHangulSyllableLast) {
            int index = static_cast<int>(it.c - kHangulSyllableFirst);
            out += initials[index / (kHangulMedials * kHangulFinals)];
            out += medials[index / kHangulFinals % kHangulMedials];
            out += finals[index % kHangulFinals];
        }
    }
}

} // namespace

std::string transliterate(const ScriptRun& run) {
    if (run.script == Script::Latin) return run.text;
    std::vector<Item> items;
    for (size_t i = 0; i < run.text.size();) {
        size_t begin = i;
        char32_t c = nextCodePoint(run.text, i);
        bool malformed = isMalformed(run.text, begin, i);
        Cls cls = classify(c, malformed);
        if (run.script == Script::Japanese && cls.kind == Kind::Letter && cls.script == Script::Han) return "";
        items.push_back({begin, i, c, cls});
    }
    std::string out;
    switch (run.script) {
    case Script::Cyrillic: transliterateCyrillic(items, run.text, out); break;
    case Script::Greek: transliterateGreek(items, run.text, out); break;
    case Script::Japanese: transliterateJapanese(items, run.text, out); break;
    case Script::Hangul: transliterateHangul(items, run.text, out); break;
    default: break;
    }
    return out;
}

namespace {

// The eSpeak voice (language code) that reads each script. Cyrillic is `ru` also for Ukrainian
// and the rest: the script does not say the language. Han without kana is read as Chinese.
const char* voiceFor(Script script) {
    switch (script) {
    case Script::Cyrillic: return "ru";
    case Script::Greek: return "el";
    case Script::Han: return "cmn";
    case Script::Japanese: return "ja";
    case Script::Hangul: return "ko";
    case Script::Hebrew: return "he";
    case Script::Arabic: return "ar";
    default: return nullptr;
    }
}

// What a run is called when it has no voice and no transliteration.
const char* nameFor(Script script) {
    switch (script) {
    case Script::Cyrillic: return "Cyrillic text";
    case Script::Greek: return "Greek text";
    case Script::Han: return "Chinese text";
    case Script::Japanese: return "Japanese text";
    case Script::Hangul: return "Korean text";
    case Script::Hebrew: return "Hebrew text";
    case Script::Arabic: return "Arabic text";
    default: return "foreign text";
    }
}

// One piece of the spoken text: `voice` is empty for text in the English voice.
struct Piece {
    std::string voice;
    std::string text;
};

// True for a byte inside a word: a letter or digit, or part of a non-ASCII character.
bool inWord(char c) {
    return static_cast<unsigned char>(c) >= 0x80 || std::isalnum(static_cast<unsigned char>(c));
}

// Pieces from different scripts can touch ("Beijing北京"): a space keeps "Beijing" and
// "Chinese text" two words.
void addPiece(std::vector<Piece>& pieces, std::string voice, std::string text) {
    if (!pieces.empty() && inWord(pieces.back().text.back()) && inWord(text.front()))
        pieces.push_back({"", " "});
    pieces.push_back({std::move(voice), std::move(text)});
}

// A run with no kanji, or a stretch of one: its voice, else its transliteration, else its name.
void addScriptPiece(std::vector<Piece>& pieces, const ScriptRun& run, const VoiceSet& voices) {
    const char* voice = voiceFor(run.script);
    if (voice && voices.count(voice)) return addPiece(pieces, voice, run.text);
    std::string latin = transliterate(run);
    if (latin.empty()) latin = nameFor(run.script);
    addPiece(pieces, "", std::move(latin));
}

// The kanji of a Japanese run are read by the `cmn` voice when it is installed (like Han with no
// kana: as Chinese), else named once; `ja` reads them letter by letter. The kana around them get
// the `ja` voice (or their transliteration).
void addJapanesePieces(std::vector<Piece>& pieces, const ScriptRun& run, const VoiceSet& voices) {
    std::string stretch;
    bool han = false, named = false;
    auto flush = [&] {
        if (stretch.empty()) return;
        ScriptRun part{Script::Japanese, stretch};
        if (han && voices.count(voiceFor(Script::Han))) {
            addPiece(pieces, voiceFor(Script::Han), stretch);
            named = false;
            stretch.clear();
            return;
        }
        bool readable = !han && (voices.count(voiceFor(Script::Japanese)) || !transliterate(part).empty());
        if (readable) {
            addScriptPiece(pieces, part, voices);
            named = false;
        } else if (!named) { // kanji, or kana with no reading: one name for both sides of a stretch
            addPiece(pieces, "", nameFor(Script::Japanese));
            named = true;
        }
        stretch.clear();
    };
    for (size_t i = 0; i < run.text.size();) {
        size_t begin = i;
        char32_t c = nextCodePoint(run.text, i);
        bool malformed = isMalformed(run.text, begin, i);
        Cls cls = classify(c, malformed);
        if (cls.kind == Kind::Letter && (cls.script == Script::Han) != han) {
            flush();
            han = cls.script == Script::Han;
        }
        stretch.append(run.text, begin, i - begin);
    }
    flush();
}

// Escapes for SSML. A malformed UTF-8 byte becomes a space: eSpeak's reading of one is not
// defined, and it must not swallow the rest of the utterance.
void appendEscaped(std::string& out, const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        size_t begin = i;
        nextCodePoint(text, i);
        if (isMalformed(text, begin, i)) {
            out += ' ';
            continue;
        }
        if (i - begin == 1) {
            switch (text[begin]) {
            case '&': out += "&amp;"; continue;
            case '<': out += "&lt;"; continue;
            case '>': out += "&gt;"; continue;
            case '"': out += "&quot;"; continue;
            case '\'': out += "&apos;"; continue;
            default: break;
            }
        }
        out.append(text, begin, i - begin);
    }
}

} // namespace

SpeechMarkup speechMarkup(std::string_view text, const VoiceSet& voices) {
    std::vector<Piece> pieces;
    for (const ScriptRun& run : scriptRuns(text)) {
        if (run.script == Script::Latin) addPiece(pieces, "", run.text);
        else if (run.script == Script::Japanese) addJapanesePieces(pieces, run, voices);
        else addScriptPiece(pieces, run, voices);
    }
    SpeechMarkup result;
    for (const Piece& p : pieces) result.ssml = result.ssml || !p.voice.empty();
    if (!result.ssml) {
        for (const Piece& p : pieces) result.text += p.text;
        return result;
    }
    for (const Piece& p : pieces) {
        if (p.voice.empty()) {
            appendEscaped(result.text, p.text);
        } else {
            result.text += "<voice xml:lang=\"" + p.voice + "\">";
            appendEscaped(result.text, p.text);
            result.text += "</voice>";
        }
    }
    return result;
}

} // namespace oap
