#include "subsonic.h"

#include "net/md5.h"
#include "net/percent_encode.h"
#include "net/subsonic_json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <random>

namespace oap {

namespace {

constexpr const char* kApiVersion = "1.16.1";
constexpr const char* kClientName = "oap";
constexpr int kSaltLength = 12; // hex digits
constexpr int kHttpOkFirst = 200, kHttpOkLast = 299;
// The OpenSubsonic extension that lets stream() start a transcode later, and its parameter (seconds).
constexpr const char* kTranscodeOffset = "transcodeOffset";
constexpr const char* kTimeOffset = "timeOffset";
// A server that answers getOpenSubsonicExtensions with an error this many times
// has no extensions (it is not OpenSubsonic); fewer could be a passing error.
constexpr int kExtensionErrorsForNo = 3;

const char* errorName(MusicServer::Error error) {
    switch (error) {
    case MusicServer::Error::None: return "none";
    case MusicServer::Error::NotConfigured: return "not configured";
    case MusicServer::Error::Unreachable: return "server not reachable";
    case MusicServer::Error::BadLogin: return "wrong user name or password";
    case MusicServer::Error::Server: return "server error";
    case MusicServer::Error::Conflict: return "newer server place";
    }
    return "unknown";
}

} // namespace

Subsonic::Config Subsonic::configFrom(const ServerConfig& server) { return {server.url, server.user, server.password}; }

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

void Subsonic::finish(std::chrono::milliseconds deadline) {
    if (!http_) return;
    http_->finish(deadline);
    // The answers to scrobbles reach their callbacks (the outbox drops what the server took); no other
    // callback runs, the program is leaving.
    std::vector<std::pair<RequestId, Reply>> batch;
    {
        std::lock_guard lock(inbox_->mutex);
        batch.swap(inbox_->ready);
    }
    for (auto& [id, reply] : batch) {
        auto it = waiting_.find(id);
        if (it == waiting_.end() || !scrobbleIds_.count(id)) continue;
        auto done = std::move(it->second);
        waiting_.erase(it);
        done(std::move(reply));
    }
}

bool Subsonic::configured() const { return !config_.url.empty(); }

MusicServer::RequestId Subsonic::playQueue(std::function<void(Error, std::optional<RemoteQueue>)> done) {
    const RequestId id = ++lastId_;
    progressChildren_[id] = request("getOpenSubsonicExtensions", {}, [](const std::string& body) {
        auto parsed = subsonic::parseExtensions(body);
        return Reply{parsed.error, std::move(parsed.items), false};
    }, [this, id, done = std::move(done)](Reply reply) mutable {
        if (!progressChildren_.contains(id)) return;
        if (reply.error == Error::Unreachable || reply.error == Error::NotConfigured || reply.error == Error::BadLogin) {
            progressChildren_.erase(id); done(reply.error, std::nullopt); return;
        }
        auto names = std::get_if<std::vector<std::string>>(&reply.data);
        bool indexed = reply.error == Error::None && names && std::find(names->begin(), names->end(), "indexBasedQueue") != names->end();
        progressChildren_[id] = request(indexed ? "getPlayQueueByIndex" : "getPlayQueue", {}, [indexed](const std::string& body) {
            auto parsed = subsonic::parsePlayQueue(body, indexed);
            return Reply{parsed.error, std::move(parsed.queue), false};
        }, [this, id, done = std::move(done)](Reply answer) mutable {
            if (!progressChildren_.erase(id)) return;
            auto* queue = std::get_if<std::optional<RemoteQueue>>(&answer.data);
            done(answer.error == Error::None && !queue ? Error::Server : answer.error, queue ? std::move(*queue) : std::nullopt);
        });
    });
    return id;
}

MusicServer::RequestId Subsonic::savePlayQueue(RemoteQueue queue, std::function<void(Error)> done) {
    const RequestId id = ++lastId_;
    progressChildren_[id] = request("getOpenSubsonicExtensions", {}, [](const std::string& body) {
        auto parsed = subsonic::parseExtensions(body);
        return Reply{parsed.error, std::move(parsed.items), false};
    }, [this, id, queue = std::move(queue), done = std::move(done)](Reply reply) mutable {
        if (!progressChildren_.contains(id)) return;
        if (reply.error == Error::Unreachable || reply.error == Error::NotConfigured || reply.error == Error::BadLogin) {
            progressChildren_.erase(id); done(reply.error); return;
        }
        auto names = std::get_if<std::vector<std::string>>(&reply.data);
        bool indexed = reply.error == Error::None && names && std::find(names->begin(), names->end(), "indexBasedQueue") != names->end();
        if ((!queue.tracks.empty() && queue.current >= queue.tracks.size()) || queue.position < 0 ||
            std::any_of(queue.tracks.begin(), queue.tracks.end(), [](const RemoteTrack& t) { return t.id.empty(); })) {
            progressChildren_.erase(id); done(Error::Server); return;
        }
        if (!indexed && !queue.tracks.empty() && std::count_if(queue.tracks.begin(), queue.tracks.end(), [&](const RemoteTrack& t) {
            return t.id == queue.tracks[queue.current].id;
        }) != 1) { progressChildren_.erase(id); done(Error::Server); return; }
        Params params;
        for (const auto& track : queue.tracks) params.emplace_back("id", track.id);
        if (!queue.tracks.empty()) {
            params.emplace_back(indexed ? "currentIndex" : "current", indexed ? std::to_string(queue.current) : queue.tracks[queue.current].id);
            params.emplace_back("position", std::to_string(queue.position));
        }
        progressChildren_[id] = request(indexed ? "savePlayQueueByIndex" : "savePlayQueue", params,
            [](const std::string& body) { return Reply{subsonic::parseStatus(body), {}, false}; },
            [this, id, done = std::move(done)](Reply answer) mutable { if (progressChildren_.erase(id)) done(answer.error); });
    });
    return id;
}

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
    return [config = std::move(config), salt = std::move(salt)](const std::string& trackId, int bitrate) -> std::string {
        if (config.url.empty()) return {};
        if (bitrate > 0) return buildUrl(config, salt(), "stream", {{"id", trackId}, {"format", "mp3"}, {"maxBitRate", std::to_string(bitrate)}});
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
    http_->get(id, url(method, params), {}, [inbox = inbox_, parse = std::move(parse), id](Http::Response response) {
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

MusicServer::RequestId Subsonic::progress(std::string albumId, std::function<void(Error, std::optional<RemoteProgress>)> done) {
    const RequestId id = ++lastId_;
    progressChildren_[id] = albumTracks(std::move(albumId), [this, id, done = std::move(done)](Error error, std::vector<RemoteTrack> tracks, bool) mutable {
        if (!progressChildren_.contains(id)) return;
        if (error != Error::None) { progressChildren_.erase(id); done(error, std::nullopt); return; }
        progressChildren_[id] = request("getBookmarks", {}, [tracks = std::move(tracks)](const std::string& body) {
            auto parsed = subsonic::parseBookmarks(body);
            std::optional<RemoteProgress> best;
            double duration = 0;
            for (const auto& track : tracks) {
                if (!std::isfinite(track.duration) || track.duration <= 0 || track.id.empty())
                    return Reply{parsed.error, std::optional<RemoteProgress>{}, false};
                duration += track.duration;
            }
            double start = 0;
            for (const auto& track : tracks) {
                for (const auto& bookmark : parsed.items) {
                    const double offset = bookmark.position / 1000.0;
                    if (bookmark.trackId == track.id && offset <= track.duration && (!best || bookmark.changed > best->updated))
                        best = RemoteProgress{start + offset, duration, false, bookmark.changed};
                }
                start += track.duration;
            }
            if (best) for (const auto& track : tracks) {
                best->trackIds.push_back(track.id);
                best->trackDurations.push_back(track.duration);
            }
            return Reply{parsed.error, std::move(best), false};
        }, [this, id, done = std::move(done)](Reply reply) mutable {
            if (!progressChildren_.erase(id)) return;
            auto* value = std::get_if<std::optional<RemoteProgress>>(&reply.data);
            done(reply.error, value ? *value : std::nullopt);
        });
    });
    return id;
}

void Subsonic::saveProgress(std::string albumId, RemoteProgress progress, std::function<void(Error)> done) {
    const RequestId id = ++lastId_;
    progressChildren_[id] = this->progress(albumId, [this, id, albumId, progress, done = std::move(done)]
        (Error error, std::optional<RemoteProgress> remote) mutable {
        if (!progressChildren_.contains(id)) return;
        if (error == Error::None && remote && !progress.trackIds.empty() &&
            (progress.trackIds != remote->trackIds || progress.trackDurations.size() != remote->trackDurations.size())) error = Error::Server;
        if (error == Error::None && remote && !progress.trackIds.empty())
            for (std::size_t i = 0; i < progress.trackDurations.size(); ++i)
                if (!std::isfinite(progress.trackDurations[i]) || std::abs(progress.trackDurations[i] - remote->trackDurations[i]) > RemoteProgress::kDurationTolerance) { error = Error::Server; break; }
        // Local positions are persisted with whole-second wall timestamps, so
        // sub-second server differences cannot establish an ordering here.
        if (error != Error::None || (remote && remote->updated / 1000 > progress.updated / 1000)) {
            progressChildren_.erase(id);
            if (done) done(error == Error::None ? Error::Conflict : error);
            return;
        }
        progressChildren_[id] = sendProgressBookmark(std::move(albumId), progress, [this, id, done = std::move(done)](Error result) mutable {
            if (progressChildren_.erase(id) && done) done(result);
        });
    });
}

MusicServer::RequestId Subsonic::sendProgressBookmark(std::string albumId, RemoteProgress progress, std::function<void(Error)> done) {
    const RequestId id = ++lastId_;
    progressChildren_[id] = albumTracks(std::move(albumId), [this, id, progress, done = std::move(done)](Error error, std::vector<RemoteTrack> tracks, bool) mutable {
        if (!progressChildren_.contains(id)) return;
        auto fail = [&](Error e) { progressChildren_.erase(id); if (done) done(e); };
        if (error != Error::None) { fail(error); return; }
        if (tracks.empty() || !std::isfinite(progress.seconds) || progress.seconds < 0) { fail(Error::Server); return; }
        if (!progress.trackIds.empty()) {
            if (progress.trackIds.size() != tracks.size() || progress.trackDurations.size() != tracks.size()) { fail(Error::Server); return; }
            for (std::size_t i = 0; i < tracks.size(); ++i)
                if (progress.trackIds[i] != tracks[i].id || !std::isfinite(progress.trackDurations[i]) ||
                    std::abs(progress.trackDurations[i] - tracks[i].duration) > RemoteProgress::kDurationTolerance) { fail(Error::Server); return; }
        }
        double remaining = progress.seconds;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const auto& track = tracks[i];
            if (track.id.empty() || !std::isfinite(track.duration) || track.duration <= 0) { fail(Error::Server); return; }
            if (remaining < track.duration || i + 1 == tracks.size()) {
                if (remaining > track.duration) { fail(Error::Server); return; }
                const auto position = static_cast<std::int64_t>(remaining * 1000);
                progressChildren_[id] = request("createBookmark", {{"id", track.id}, {"position", std::to_string(position)}},
                    [](const std::string& body) { return Reply{subsonic::parseStatus(body), {}, false}; },
                    [this, id, done = std::move(done)](Reply reply) mutable {
                        if (progressChildren_.erase(id) && done) done(reply.error);
                    });
                return;
            }
            remaining -= track.duration;
        }
    });
    return id;
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

// getAlbum is not paged, so albumTracks()'s `more` is always false and is dropped here.
MusicServer::RequestId Subsonic::book(std::string albumId, std::function<void(Error, RemoteBook)> done) {
    return albumTracks(std::move(albumId), [done = std::move(done)](Error error, std::vector<RemoteTrack> tracks, bool) {
        done(error, RemoteBook{std::move(tracks), {}});
    });
}

MusicServer::RequestId Subsonic::inProgress(Done<RemoteAlbum> done) {
    RequestId id = ++lastId_;
    waiting_[id] = [done = std::move(done)](Reply reply) { done(reply.error, {}, false); };
    std::lock_guard lock(inbox_->mutex);
    inbox_->ready.emplace_back(id, Reply{configured() ? Error::Server : Error::NotConfigured, {}, false});
    return id;
}

MusicServer::RequestId Subsonic::playlists(Done<RemotePlaylist> done) {
    return list<RemotePlaylist>("getPlaylists", {}, subsonic::parsePlaylists, std::move(done));
}

MusicServer::RequestId Subsonic::trackDetails(std::string trackId, Done<RemoteTrack> done) {
    return list<RemoteTrack>("getSong", {{"id", std::move(trackId)}}, subsonic::parseSong, std::move(done));
}
MusicServer::RequestId Subsonic::starredTracks(Done<RemoteTrack> done) {
    return list<RemoteTrack>("getStarred2", {}, subsonic::parseStarred, std::move(done));
}
MusicServer::RequestId Subsonic::setStarred(std::string trackId, bool starred, std::function<void(Error)> done) {
    return request(starred ? "star" : "unstar", {{"id", std::move(trackId)}},
                   [](const std::string& body) { return Reply{subsonic::parseStatus(body), {}, false}; },
                   [done = std::move(done)](Reply reply) { done(reply.error); });
}
MusicServer::RequestId Subsonic::playlistTracks(std::string playlistId, Done<RemoteTrack> done) {
    return list<RemoteTrack>("getPlaylist", {{"id", std::move(playlistId)}}, subsonic::parsePlaylist, std::move(done));
}

MusicServer::RequestId Subsonic::createPlaylist(std::string name, std::vector<std::string> trackIds,
                                                std::function<void(Error, std::string)> done) {
    if (configured() && trackIds.size() > kMaxPlaylistTracks) { // the URL would be too long: say so, send nothing
        RequestId id = ++lastId_;
        waiting_.emplace(id, [done = std::move(done)](Reply) { done(Error::Server, {}); });
        std::lock_guard lock(inbox_->mutex);
        inbox_->ready.emplace_back(id, Reply{Error::Server, {}, false});
        return id;
    }
    Params params = {{"name", std::move(name)}};
    for (std::string& trackId : trackIds) params.emplace_back("songId", std::move(trackId));
    return request(
        "createPlaylist", params,
        [](const std::string& body) {
            auto result = subsonic::parseCreatedPlaylist(body);
            return Reply{result.error, std::move(result.id), false};
        },
        [done = std::move(done)](Reply reply) {
            auto* id = std::get_if<std::string>(&reply.data);
            done(reply.error, reply.error == Error::None && id ? std::move(*id) : std::string());
        });
}

MusicServer::RequestId Subsonic::addPlaylistTrack(std::string playlistId, std::string trackId, std::function<void(Error)> done) {
    return request("updatePlaylist", {{"playlistId", std::move(playlistId)}, {"songIdToAdd", std::move(trackId)}},
                   [](const std::string& body) { return Reply{subsonic::parseStatus(body), {}, false}; },
                   [done = std::move(done)](Reply reply) { done(reply.error); });
}
MusicServer::RequestId Subsonic::removePlaylistTrack(std::string playlistId, std::size_t index, std::function<void(Error)> done) {
    return request("updatePlaylist", {{"playlistId", std::move(playlistId)}, {"songIndexToRemove", std::to_string(index)}},
                   [](const std::string& body) { return Reply{subsonic::parseStatus(body), {}, false}; },
                   [done = std::move(done)](Reply reply) { done(reply.error); });
}
MusicServer::RequestId Subsonic::deletePlaylist(std::string playlistId, std::function<void(Error)> done) {
    return request("deletePlaylist", {{"id", std::move(playlistId)}},
                   [](const std::string& body) { return Reply{subsonic::parseStatus(body), {}, false}; },
                   [done = std::move(done)](Reply reply) { done(reply.error); });
}

void Subsonic::cancel(RequestId id) {
    if (auto it = progressChildren_.find(id); it != progressChildren_.end()) {
        const auto child = it->second;
        progressChildren_.erase(it);
        cancel(child);
        return;
    }
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

void Subsonic::scrobble(const std::string& trackId, bool submission, std::optional<std::int64_t> timeMs,
                        std::function<void(Error)> done) {
    if (!configured()) return;
    Params params = {{"id", trackId}, {"submission", submission ? "true" : "false"}};
    if (timeMs) params.emplace_back("time", std::to_string(*timeMs));
    const RequestId id = request(
        "scrobble", std::move(params),
        [](const std::string& body) { return Reply{subsonic::parseStatus(body), {}, false}; },
        [this, id = lastId_ + 1, done = std::move(done)](Reply reply) {
            scrobbleIds_.erase(id);
            if (reply.error != Error::None) std::fprintf(stderr, "server: scrobble failed: %s\n", errorName(reply.error));
            if (done) done(reply.error);
        });
    scrobbleIds_.insert(id);
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
