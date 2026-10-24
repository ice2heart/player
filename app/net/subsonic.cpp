#include "subsonic.h"

#include "core/settings.h"
#include "net/md5.h"
#include "net/subsonic_json.h"

#include <algorithm>
#include <cctype>
#include <random>

namespace oap {

namespace {

constexpr const char* kApiVersion = "1.16.1";
constexpr const char* kClientName = "oap";
constexpr const char* kServerType = "subsonic"; // server.type in config.yaml
constexpr int kSaltLength = 12; // hex digits
constexpr int kHttpOkFirst = 200, kHttpOkLast = 299;

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

} // namespace

Subsonic::Config Subsonic::configFrom(const Settings& config) {
    Config c;
    c.url = config.getString("server.url");
    c.user = config.getString("server.user");
    c.password = config.getString("server.password");
    c.format = config.getString("server.format");
    c.maxBitrate = config.getInt("server.max_bitrate", 0);
    return c;
}

std::unique_ptr<Subsonic> Subsonic::fromConfig(const Settings& config) {
    std::string type = config.getString("server.type", kServerType);
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (type != kServerType || !serverConfigured(config)) return nullptr;
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
    while (!config_.url.empty() && config_.url.back() == '/') config_.url.pop_back();
}

Subsonic::Subsonic(Config config) : Subsonic(std::move(config), std::make_unique<HttpClient>()) {}

Subsonic::~Subsonic() {
    http_.reset(); // stop the HTTP thread first: its handlers hold the inbox
}

bool Subsonic::configured() const { return !config_.url.empty(); }

std::string Subsonic::url(const std::string& method, const Params& params) const {
    std::string salt = salt_();
    std::string out = config_.url + "/rest/" + method + "?u=" + percentEncode(config_.user) +
                      "&t=" + md5Hex(config_.password + salt) + "&s=" + percentEncode(salt) + "&v=" + kApiVersion +
                      "&c=" + kClientName + "&f=json";
    for (const auto& [key, value] : params) out += "&" + key + "=" + percentEncode(value);
    return out;
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

MusicServer::RequestId Subsonic::albums(std::string listType, int offset, int size, Done<RemoteAlbum> done) {
    int pageSize = std::clamp(size, 1, kMaxPageSize); // a larger page would come back short and look like the last
    Params params = {{"type", std::move(listType)},
                     {"size", std::to_string(pageSize)},
                     {"offset", std::to_string(std::max(offset, 0))}};
    return list<RemoteAlbum>(
        "getAlbumList2", params, [pageSize](const std::string& body) { return subsonic::parseAlbumList(body, pageSize); },
        std::move(done));
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

void Subsonic::cancel(RequestId id) {
    if (waiting_.erase(id)) http_->cancel(id);
}

std::string Subsonic::streamUrl(const std::string& trackId) const {
    if (!configured()) return {};
    Params params = {{"id", trackId}};
    if (!config_.format.empty() && config_.format != "raw") params.emplace_back("format", config_.format);
    if (config_.maxBitrate > 0) params.emplace_back("maxBitRate", std::to_string(config_.maxBitrate));
    return url("stream", params);
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
