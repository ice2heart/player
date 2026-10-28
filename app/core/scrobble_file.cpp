#include "core/scrobble_file.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace oap {

namespace {

constexpr const char* kMagic = "oap-scrobbles";
constexpr const char* kVersion = "1";
constexpr std::size_t kMaxTimeDigits = 18; // a long long holds 18 digits whatever they are

// A tab or line break in an id would split the line: they are written as \t \n \r, and \ as \\.
std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        out += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return out;
}

} // namespace

std::string formatScrobbles(const std::vector<ScrobbleEntry>& entries) {
    if (entries.empty()) return "";
    std::string out = std::string(kMagic) + "\t" + kVersion + "\n";
    for (const ScrobbleEntry& e : entries) out += escape(e.server) + "\t" + escape(e.trackId) + "\t" + std::to_string(e.timeMs) + "\n";
    return out;
}

std::vector<ScrobbleEntry> parseScrobbles(const std::string& text) {
    std::vector<ScrobbleEntry> entries;
    if (text.size() > kMaxScrobbleFileBytes) return entries;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != std::string(kMagic) + "\t" + kVersion) return entries;
    while (std::getline(in, line)) {
        const std::size_t a = line.find('\t');
        const std::size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        const std::string time = line.substr(b + 1);
        // Digits only (no sign or blanks), and short enough to fit: a line that is not ours is skipped.
        if (time.empty() || time.size() > kMaxTimeDigits || time.find_first_not_of("0123456789") != std::string::npos) continue;
        const long long ms = std::strtoll(time.c_str(), nullptr, 10);
        if (ms <= 0) continue;
        ScrobbleEntry e{unescape(line.substr(0, a)), unescape(line.substr(a + 1, b - a - 1)), ms};
        if (e.server.empty() || e.trackId.empty()) continue;
        entries.push_back(std::move(e));
    }
    if (entries.size() > kMaxScrobbles) entries.erase(entries.begin(), entries.end() - kMaxScrobbles);
    return entries;
}

} // namespace oap
