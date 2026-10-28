#include "subsonic.h"

#include "core/settings.h"
#include "net/md5.h"
#include "net/subsonic_json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>

namespace oap {

namespace {

constexpr const char* kApiVersion = "1.16.1";
constexpr const char* kClientName = "oap";
constexpr const char* kServerType = "subsonic"; // server.type in config.yaml
constexpr int kSaltLength = 12; // hex digits
constexpr int kHttpOkFirst = 200, kHttpOkLast = 299;
// The OpenSubsonic extension that lets stream() start a transcode later, and its parameter (seconds).
constexpr const char* kTranscodeOffset = "transcodeOffset";
constexpr const char* kTimeOffset = "timeOffset";
// A server that answers getOpenSubsonicExtensions with an error this many times
// has no extensions (it is not OpenSubsonic); fewer could be a passing error.
constexpr int kExtensionErrorsForNo = 3;

// RFC 3986: everything but the unreserved characters is %XX-encoded, so ids,
// names and passwords with '&', '=', '/', spaces or UTF-8 survive.
std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) && c < 0x80) out += char(c);
        else if (c == '-' || c == '.' || c == '_' || c == '~') out += char(c);
        else out += '%', out += hex[c >> 4], out += hex[c & 0xf];
    }
    return out;
}

void trimTrailingSlashes(std::string& url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
}

const char* errorName(MusicServer::Error error) {
    switch (error) {
    case MusicServer::Error::None: return "none";
    case MusicServer::Error::NotConfigured: return "not configured";
    case MusicServer::Error::Unreachable: return "server not reachable";
    case MusicServer::Error::BadLogin: return "wrong user name or password";
    case MusicServer::Error::Server: return "server error";
    }
    return "unknown";
}

} // namespace

Subsonic::Config Subsonic::configFrom(const Settings& config) {
    Config c;
    c.url = config.getString("server.url");
    c.user = config.getString("server.user");
    c.password = config.getString("server.password");
    return c;
}

bool Subsonic::handles(const Settings& config) {
    std::string type = config.getString("server.type", kServerType);
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return type == kServerType && serverConfigured(config);
}

std::unique_ptr<Subsonic> Subsonic::fromConfig(const Settings& config) {
    if (!handles(config)) return nullptr;
    return std::make_unique<Subsonic>(configFrom(config));
}

std::string Subsonic::randomSalt() {
    thread_local std::mt19937_64 generator = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    std::uniform_int_distribution<int> digit(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string salt;
    for (int i = 0; i < kSaltLength; ++i) salt += hex[digit(generator)];
    return salt;
}

Subsonic::Subsonic(Config config, std::unique_ptr<Http> http, SaltSource salt)
    : config_(std::move(config)), salt_(std::move(salt)), http_(std::move(http)) {
    trimTrailingSlashes(config_.url);
}

Subsonic::Subsonic(Config config) : Subsonic(std::move(config), std::make_unique<HttpClient>()) {}

Subsonic::~Subsonic() {
    http_.reset(); // stop the HTTP thread first: its handlers hold the inbox
}

bool Subsonic::configured() const { return !config_.url.empty(); }

std::string Subsonic::buildUrl(const Config& config, const std::string& salt, const std::string& method,
                               const Params& params) {
    std::string out = config.url + "/rest/" + method + "?u=" + percentEncode(config.user) +
                      "&t=" + md5Hex(config.password + salt) + "&s=" + percentEncode(salt) + "&v=" + kApiVersion +
                      "&c=" + kClientName + "&f=json";
    for (const auto& [key, value] : params) out += "&" + key + "=" + percentEncode(value);
    return out;
}

std::string Subsonic::url(const std::string& method, const Params& params) const {
    return buildUrl(config_, salt_(), method, params);
}

Subsonic::DownloadUrls Subsonic::downloadUrls(Config config, SaltSource salt) {
    trimTrailingSlashes(config.url);
    return [config = std::move(config), salt = std::move(salt)](const std::string& trackId) -> std::string {
        if (config.url.empty()) return {};
        return buildUrl(config, salt(), "download", {{"id", trackId}});
    };
}

MusicServer::RequestId Subsonic::request(const std::string& method, const Params& params, Parser parse,
                                         std::function<void(Reply)> done) {
    RequestId id = ++lastId_;
    waiting_.emplace(id, std::move(done));
    if (!configured()) {
        std::lock_guard lock(inbox_->mutex);
        inbox_->ready.emplace_back(id, Reply{Error::NotConfigured, {}, false});
        return id;
    }
    http_->get(id, url(method, params), [inbox = inbox_, parse = std::move(parse), id](Http::Response response) {
        Reply reply;
        if (!response.reached) {
            reply.error = Error::Unreachable;
        } else {
            reply = parse(response.body); // a Subsonic error in the body is the better answer
            if (reply.error == Error::None && (response.status < kHttpOkFirst || response.status > kHttpOkLast))
                reply = Reply{Error::Server, {}, false};
        }
        std::lock_guard lock(inbox->mutex);
        inbox->ready.emplace_back(id, std::move(reply));
    });
    return id;
}

template <class T, class P>
MusicServer::RequestId Subsonic::list(const std::string& method, const Params& params, P parse, Done<T> done) {
    return request(
        method, params,
        [parse](const std::string& body) {
            auto parsed = parse(body);
            return Reply{parsed.error, std::move(parsed.items), parsed.more};
        },
        [done = std::move(done)](Reply reply) {
            auto* items = std::get_if<std::vector<T>>(&reply.data);
            if (reply.error != Error::None || !items) done(reply.error, {}, false);
            else done(Error::None, std::move(*items), reply.more);
        });
}

MusicServer::RequestId Subsonic::ping(std::function<void(Error, ServerInfo)> done) {
    return request(
        "ping", {},
        [](const std::string& body) {
            auto result = subsonic::parsePing(body);
            return Reply{result.error, std::move(result.info), false};
        },
        [done = std::move(done)](Reply reply) {
            auto* info = std::get_if<ServerInfo>(&reply.data);
            if (reply.error != Error::None || !info) done(reply.error, {});
            else done(Error::None, std::move(*info));
        });
}

MusicServer::RequestId Subsonic::catalogueVersion(std::function<void(Error, std::string)> done) {
    return request(
        "getArtists", {},
        [](const std::string& body) {
            auto result = subsonic::parseCatalogueVersion(body);
            return Reply{result.error, std::move(result.version), false};
        },
        [done = std::move(done)](Reply reply) {
            auto* version = std::get_if<std::string>(&reply.data);
            if (reply.error != Error::None || !version) done(reply.error == Error::None ? Error::Server : reply.error, {});
            else done(Error::None, std::move(*version));
        });
}

MusicServer::RequestId Subsonic::albums(std::string listType, int offset, int size, Done<RemoteAlbum> done,
                                        std::string musicFolderId) {
    int pageSize = std::clamp(size, 1, kMaxPageSize); // a larger page would come back short and look like the last
    Params params = {{"type", std::move(listType)},
                     {"size", std::to_string(pageSize)},
                     {"offset", std::to_string(std::max(offset, 0))}};
    if (!musicFolderId.empty()) params.emplace_back("musicFolderId", std::move(musicFolderId));
    return list<RemoteAlbum>(
        "getAlbumList2", params, [pageSize](const std::string& body) { return subsonic::parseAlbumList(body, pageSize); },
        std::move(done));
}

MusicServer::RequestId Subsonic::musicFolders(Done<RemoteFolder> done) {
    return list<RemoteFolder>("getMusicFolders", {}, subsonic::parseMusicFolders, std::move(done));
}

MusicServer::RequestId Subsonic::artists(Done<RemoteArtist> done) {
    return list<RemoteArtist>("getArtists", {}, subsonic::parseArtists, std::move(done));
}

MusicServer::RequestId Subsonic::artistAlbums(std::string artistId, Done<RemoteAlbum> done) {
    return list<RemoteAlbum>("getArtist", {{"id", std::move(artistId)}}, subsonic::parseArtist, std::move(done));
}

MusicServer::RequestId Subsonic::albumTracks(std::string albumId, Done<RemoteTrack> done) {
    return list<RemoteTrack>("getAlbum", {{"id", std::move(albumId)}}, subsonic::parseAlbum, std::move(done));
}

MusicServer::RequestId Subsonic::playlists(Done<RemotePlaylist> done) {
    return list<RemotePlaylist>("getPlaylists", {}, subsonic::parsePlaylists, std::move(done));
}

MusicServer::RequestId Subsonic::playlistTracks(std::string playlistId, Done<RemoteTrack> done) {
    return list<RemoteTrack>("getPlaylist", {{"id", std::move(playlistId)}}, subsonic::parsePlaylist, std::move(done));
}

MusicServer::RequestId Subsonic::createPlaylist(std::string name, std::vector<std::string> trackIds,
                                                std::function<void(Error)> done) {
    if (configured() && trackIds.size() > kMaxPlaylistTracks) { // the URL would be too long: say so, send nothing
        RequestId id = ++lastId_;
        waiting_.emplace(id, [done = std::move(done)](Reply) { done(Error::Server); });
        std::lock_guard lock(inbox_->mutex);
        inbox_->ready.emplace_back(id, Reply{Error::Server, {}, false});
        return id;
    }
    Params params = {{"name", std::move(name)}};
    for (std::string& trackId : trackIds) params.emplace_back("songId", std::move(trackId));
    return request(
        "createPlaylist", params, [](const std::string& body) { return Reply{subsonic::parseStatus(body), {}, false}; },
        [done = std::move(done)](Reply reply) { done(reply.error); });
}

void Subsonic::cancel(RequestId id) {
    if (waiting_.erase(id)) http_->cancel(id);
}

MusicServer::Stream Subsonic::stream(const std::string& trackId, int maxBitRate) {
    if (!configured()) return {};
    Params params = {{"id", trackId}};
    if (maxBitRate <= 0) return {url("stream", params), std::nullopt}; // the original file: it seeks with Range
    if (transcodeOffset_ == Known::NotYet) askExtensions();
    params.emplace_back("format", kTranscodeFormat);
    params.emplace_back("maxBitRate", std::to_string(maxBitRate));
    Stream stream{url("stream", params), Transcoded{}};
    if (transcodeOffset_ == Known::Yes) stream.transcoded->offsetUrl = stream.url + "&" + kTimeOffset + "=";
    return stream;
}

void Subsonic::scrobble(const std::string& trackId, bool submission) {
    if (!configured()) return;
    request(
        "scrobble", {{"id", trackId}, {"submission", submission ? "true" : "false"}},
        [](const std::string& body) { return Reply{subsonic::parseStatus(body), {}, false}; },
        [](Reply reply) {
            if (reply.error != Error::None) std::fprintf(stderr, "server: scrobble failed: %s\n", errorName(reply.error));
        });
}

void Subsonic::askExtensions() {
    transcodeOffset_ = Known::Asking;
    request(
        "getOpenSubsonicExtensions", {},
        [](const std::string& body) {
            auto parsed = subsonic::parseExtensions(body);
            return Reply{parsed.error, std::move(parsed.items), false};
        },
        [this](Reply reply) {
            auto* names = std::get_if<std::vector<std::string>>(&reply.data);
            if (reply.error == Error::None) {
                bool has = names && std::find(names->begin(), names->end(), kTranscodeOffset) != names->end();
                transcodeOffset_ = has ? Known::Yes : Known::No;
            } else if (reply.error != Error::Unreachable && ++extensionErrors_ >= kExtensionErrorsForNo) {
                transcodeOffset_ = Known::No; // a server without OpenSubsonic, not a passing error
            } else {
                transcodeOffset_ = Known::NotYet; // ask again with the next transcode
            }
        });
}

void Subsonic::poll() {
    std::vector<std::pair<RequestId, Reply>> batch;
    {
        std::lock_guard lock(inbox_->mutex);
        batch.swap(inbox_->ready);
    }
    // A callback may cancel a later request of this batch, or make new ones.
    for (auto& [id, reply] : batch) {
        auto it = waiting_.find(id);
        if (it == waiting_.end()) continue; // cancelled
        auto done = std::move(it->second);
        waiting_.erase(it);
        done(std::move(reply));
    }
}

} // namespace oap
