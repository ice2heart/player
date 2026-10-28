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

} // namespace oap
