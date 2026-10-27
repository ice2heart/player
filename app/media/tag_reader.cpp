#include "tag_reader.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace oap {

namespace {

// A key as FFmpeg's demuxers spell it differs by format ("album_artist", "ALBUMARTIST",
// "Album Artist"), so keys are compared in lower case without spaces, "_" and "-".
// ASCII only: the values are UTF-8, and the C locale functions may take its bytes for letters.
bool isAsciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

std::string normalizeKey(const std::string& key) {
    std::string out;
    for (char c : key) {
        if (c == ' ' || c == '_' || c == '-') continue;
        out.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
    }
    return out;
}

std::string trim(const std::string& s) {
    auto begin = std::find_if_not(s.begin(), s.end(), isAsciiSpace);
    auto end = std::find_if_not(s.rbegin(), s.rend(), isAsciiSpace).base();
    return begin < end ? std::string(begin, end) : std::string();
}

// The tags of the container and of the audio stream; the container's come first.
class TagSet {
public:
    void add(const AVDictionary* dict) {
        const AVDictionaryEntry* entry = nullptr;
        while ((entry = av_dict_iterate(dict, entry))) {
            std::string value = trim(entry->value);
            if (!value.empty()) entries_.emplace_back(normalizeKey(entry->key), std::move(value));
        }
    }
    // The first non-empty value of the first of `keys` that has one.
    std::string get(std::initializer_list<const char*> keys) const {
        for (const char* key : keys)
            for (const auto& [k, v] : entries_)
                if (k == key) return v;
        return {};
    }

private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

// The leading digits: "3/12" is 3, "07" is 7. 0 when there are none or too many.
int leadingNumber(const std::string& text, size_t maxDigits) {
    size_t n = 0;
    while (n < text.size() && isAsciiDigit(text[n])) ++n;
    if (n == 0 || n > maxDigits) return 0;
    return std::stoi(text.substr(0, n));
}

// "2019", "2019-05-03" and "2019-05-03T10:00:00" all give 2019. Anything that does not start
// with four digits and then end or go on with a non-digit ("05/03/2019", "19", "20190503") gives 0.
int yearOf(const std::string& date) {
    constexpr size_t kYearDigits = 4;
    size_t n = 0;
    while (n < date.size() && isAsciiDigit(date[n])) ++n;
    return n == kYearDigits ? std::stoi(date.substr(0, n)) : 0;
}

struct FormatCloser {
    void operator()(AVFormatContext* fmt) const { avformat_close_input(&fmt); }
};

} // namespace

std::optional<TrackTags> FfmpegTagReader::read(const std::filesystem::path& file) {
    // FFmpeg takes the name for a URL: "file:a.mp3" relative would be a protocol prefix.
    const std::string path = std::filesystem::absolute(file).string();
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0) return std::nullopt; // frees raw
    std::unique_ptr<AVFormatContext, FormatCloser> fmt(raw);
    if (avformat_find_stream_info(fmt.get(), nullptr) < 0) return std::nullopt;
    int index = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0) return std::nullopt;

    TagSet set;
    set.add(fmt->metadata);
    set.add(fmt->streams[index]->metadata);

    TrackTags tags;
    tags.title = set.get({"title"});
    tags.artist = set.get({"artist"});
    tags.albumArtist = set.get({"albumartist"});
    tags.album = set.get({"album"});
    tags.genre = set.get({"genre"});
    tags.musicBrainzAlbumId = set.get({"musicbrainzalbumid"});
    constexpr size_t kNumberDigits = 5;
    tags.track = leadingNumber(set.get({"track"}), kNumberDigits);
    tags.disc = leadingNumber(set.get({"disc"}), kNumberDigits);
    tags.year = yearOf(set.get({"date", "year"}));
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) tags.duration = double(fmt->duration) / AV_TIME_BASE;
    return tags;
}

} // namespace oap
