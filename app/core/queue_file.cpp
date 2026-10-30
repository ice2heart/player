#include "core/queue_file.h"

#include <cmath>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace oap {

namespace {

constexpr const char* kMagic = "oap-queue";
constexpr const char* kVersion = "1";

// A tab or line break in a title would split the line: they are written as \t \n \r, and \ as \\.
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

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t from = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', from);
        fields.push_back(line.substr(from, tab == std::string::npos ? tab : tab - from));
        if (tab == std::string::npos) return fields;
        from = tab + 1;
    }
}

std::string seconds(double s) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", std::isfinite(s) && s > 0 ? s : 0.0);
    return buf;
}

// A number of seconds; 0 for anything else.
double parseSeconds(const std::string& s) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return end != s.c_str() && std::isfinite(v) && v > 0 ? v : 0.0;
}

std::optional<std::size_t> parseIndex(const std::string& s) {
    if (s.empty() || s.size() > 9 || s.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return static_cast<std::size_t>(std::stoul(s));
}

std::optional<std::int64_t> parseTimestamp(const std::string& s) {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (error != std::errc{} || end != s.data() + s.size() || value < 0) return std::nullopt;
    return value;
}

const char* shuffleName(QueueShuffle s) { return s == QueueShuffle::Tracks ? "tracks" : s == QueueShuffle::Albums ? "albums" : "off"; }
const char* repeatName(QueueRepeat r) {
    return r == QueueRepeat::All ? "all" : r == QueueRepeat::Album ? "album" : r == QueueRepeat::Track ? "track" : "off";
}

} // namespace

std::string formatQueue(const SavedQueue& saved) {
    if (saved.queue.empty() && saved.changed == 0 && saved.pendingServers.empty()) return {};
    const Queue queue = saved.queue.around(kMaxSavedEntries);
    std::string out = std::string(kMagic) + '\t' + kVersion + '\n';
    out += "title\t" + escape(saved.title) + '\n';
    out += std::string("shuffle\t") + shuffleName(queue.shuffle()) + '\n';
    out += std::string("repeat\t") + repeatName(queue.repeat()) + '\n';
    out += std::string("edited\t") + (queue.edited() ? "1" : "0") + '\n';
    out += "position\t" + std::to_string(queue.position()) + '\n';
    out += "seconds\t" + seconds(saved.position) + '\n';
    if (saved.changed > 0) out += "changed\t" + std::to_string(saved.changed) + '\n';
    for (const auto& key : saved.pendingServers) out += "pending\t" + escape(key) + '\n';
    for (const auto& [key, stamp] : saved.acknowledgedServers) out += "ack\t" + escape(key) + '\t' + std::to_string(stamp) + '\n';
    out += "order";
    for (std::size_t i : queue.playOrder()) out += '\t' + std::to_string(i);
    out += '\n';
    for (const QueueEntry& e : queue.originalEntries()) {
        if (e.kind == QueueEntry::Kind::Local) {
            out += "L\t" + escape(e.path) + '\t' + escape(e.title) + '\t' + escape(e.album) + '\t' + escape(e.artist) + '\n';
        } else {
            out += "R\t" + escape(e.serverUrl) + '\t' + escape(e.user) + '\t' + escape(e.trackId) + '\t' + escape(e.albumId) + '\t' +
                   escape(e.title) + '\t' + escape(e.album) + '\t' + escape(e.artist) + '\t' + seconds(e.duration) + '\n';
        }
    }
    return out;
}

std::optional<SavedQueue> parseQueue(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || split(line) != std::vector<std::string>{kMagic, kVersion}) return std::nullopt;
    SavedQueue saved;
    std::vector<QueueEntry> entries;
    std::vector<std::size_t> order;
    std::size_t position = 0;
    QueueShuffle shuffle = QueueShuffle::Off;
    QueueRepeat repeat = QueueRepeat::Off;
    bool edited = false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = split(line);
        const std::string& key = f[0];
        if (key == "L" || key == "R") {
            if (entries.size() >= kMaxSavedEntries) return std::nullopt; // not something this app wrote
            QueueEntry e;
            if (key == "L" && f.size() == 5) {
                e.path = unescape(f[1]), e.title = unescape(f[2]), e.album = unescape(f[3]), e.artist = unescape(f[4]);
            } else if (key == "R" && f.size() == 9) {
                e.kind = QueueEntry::Kind::Remote;
                e.serverUrl = unescape(f[1]), e.user = unescape(f[2]), e.trackId = unescape(f[3]), e.albumId = unescape(f[4]);
                e.title = unescape(f[5]), e.album = unescape(f[6]), e.artist = unescape(f[7]);
                e.duration = parseSeconds(f[8]);
            } else {
                return std::nullopt;
            }
            entries.push_back(std::move(e));
        } else if (key == "title" && f.size() == 2) {
            saved.title = unescape(f[1]);
        } else if (key == "shuffle" && f.size() == 2) {
            shuffle = f[1] == "tracks" ? QueueShuffle::Tracks : f[1] == "albums" ? QueueShuffle::Albums : QueueShuffle::Off;
        } else if (key == "repeat" && f.size() == 2) {
            repeat = f[1] == "all"     ? QueueRepeat::All
                     : f[1] == "album" ? QueueRepeat::Album
                     : f[1] == "track" ? QueueRepeat::Track
                                       : QueueRepeat::Off;
        } else if (key == "edited" && f.size() == 2) {
            edited = f[1] == "1";
        } else if (key == "position" && f.size() == 2) {
            position = parseIndex(f[1]).value_or(std::size_t(-1)); // not a number: no entry is current
        } else if (key == "seconds" && f.size() == 2) {
            saved.position = parseSeconds(f[1]);
        } else if (key == "changed" && f.size() == 2) {
            auto stamp = parseTimestamp(f[1]);
            if (!stamp) return std::nullopt;
            saved.changed = *stamp;
        } else if (key == "pending" && f.size() == 2) {
            saved.pendingServers.push_back(unescape(f[1]));
        } else if (key == "ack" && f.size() == 3) {
            auto stamp = parseTimestamp(f[2]);
            if (!stamp) return std::nullopt;
            saved.acknowledgedServers.emplace_back(unescape(f[1]), *stamp);
        } else if (key == "order") {
            for (std::size_t i = 1; i < f.size() && order.size() <= kMaxSavedEntries; ++i)
                order.push_back(parseIndex(f[i]).value_or(kMaxSavedEntries + 1)); // out of range: repaired below
        }
    }
    if (entries.empty()) return saved.changed > 0 || !saved.pendingServers.empty() ? std::optional<SavedQueue>(std::move(saved)) : std::nullopt;
    const bool kept = saved.queue.restore(std::move(entries), std::move(order), position, shuffle, repeat, edited);
    if (!kept || saved.queue.position() != position) saved.position = 0; // another entry is current: the seconds were not its own
    return saved;
}

} // namespace oap
