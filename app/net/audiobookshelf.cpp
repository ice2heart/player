#include "audiobookshelf.h"

#include "net/audiobookshelf_json.h"
#include "net/percent_encode.h"

#include <algorithm>
#include <cstdio>

namespace oap {

namespace {

constexpr int kHttpOkFirst = 200, kHttpOkLast = 299, kHttpUnauthorized = 401;
constexpr int kHttpNotFound = 404;
constexpr char kIdSeparator = '/'; // RemoteTrack ids are "<item id>/<ino>"
constexpr const char* kNewest = "newest";
constexpr const char* kAlphabetical = "alphabeticalByName";

} // namespace

Audiobookshelf::Config Audiobookshelf::configFrom(const ServerConfig& server) {
    return {server.url, server.user, server.apiKey, server.library};
}

Audiobookshelf::Audiobookshelf(Config config, std::unique_ptr<Http> http)
    : config_(std::move(config)), http_(std::move(http)) {
    trimTrailingSlashes(config_.url);
}

Audiobookshelf::Audiobookshelf(Config config) : Audiobookshelf(std::move(config), std::make_unique<HttpClient>()) {}

Audiobookshelf::~Audiobookshelf() {
    http_.reset(); // stop the HTTP thread first: its handlers hold the inbox
}

void Audiobookshelf::finishTransport(std::chrono::milliseconds deadline) {
    if (transportFinished_) return;
    http_->finish(deadline);
    transportFinished_ = true;
}

void Audiobookshelf::finish(std::chrono::milliseconds deadline) {
    finishTransport(deadline);
    std::vector<std::pair<RequestId, Reply>> batch;
    {
        std::lock_guard lock(inbox_->mutex);
        batch.swap(inbox_->ready);
    }
    for (auto& [id, reply] : batch) {
        auto it = waiting_.find(id);
        if (it == waiting_.end() || !progressSaveIds_.contains(id)) continue;
        auto done = std::move(it->second);
        waiting_.erase(it);
        done(std::move(reply));
    }
    waiting_.clear(); // an aborted or browsing callback cannot run after finish
    progressSaveIds_.clear();
}

bool Audiobookshelf::configured() const { return !config_.url.empty() && !config_.apiKey.empty(); }

void Audiobookshelf::answer(RequestId id, std::function<void(Reply)> done, Reply reply) {
    waiting_[id] = std::move(done);
    std::lock_guard lock(inbox_->mutex);
    inbox_->ready.emplace_back(id, std::move(reply));
}

void Audiobookshelf::send(RequestId id, const std::string& path, Parser parse, std::function<void(Reply)> done,
                         std::optional<std::string> patchBody, bool missingProgressIsNone) {
    if (!configured()) return answer(id, std::move(done), Reply{Error::NotConfigured, {}, false});
    waiting_[id] = std::move(done);
    auto handler = [inbox = inbox_, parse = std::move(parse), id, missingProgressIsNone](Http::Response response) {
        Reply reply;
        if (!response.reached) reply.error = Error::Unreachable;
        else if (response.status == kHttpUnauthorized) reply.error = Error::BadLogin;
        else if (response.status == kHttpNotFound && missingProgressIsNone)
            reply.data = std::optional<RemoteProgress>{};
        else if (response.status < kHttpOkFirst || response.status > kHttpOkLast) reply.error = Error::Server;
        else reply = parse(response.body);
        std::lock_guard lock(inbox->mutex);
        inbox->ready.emplace_back(id, std::move(reply));
    };
    Http::Headers headers{"Authorization: Bearer " + config_.apiKey};
    if (patchBody) http_->patch(id, config_.url + path, std::move(headers), std::move(*patchBody), std::move(handler));
    else http_->get(id, config_.url + path, std::move(headers), std::move(handler));
}

template <class T, class P>
void Audiobookshelf::sendList(RequestId id, const std::string& path, P parse, Done<T> done) {
    send(
        id, path,
        [parse](const std::string& body) {
            auto parsed = parse(body);
            return Reply{parsed.error, std::move(parsed.items), parsed.more};
        },
        [done = std::move(done)](Reply reply) {
            auto* items = std::get_if<std::vector<T>>(&reply.data);
            if (reply.error != Error::None || !items) done(reply.error == Error::None ? Error::Server : reply.error, {}, false);
            else done(Error::None, std::move(*items), reply.more);
        });
}

template <class T> MusicServer::RequestId Audiobookshelf::noItems(Done<T> done) {
    RequestId id = ++lastId_;
    answer(id, [done = std::move(done)](Reply reply) { done(reply.error, {}, false); },
           Reply{configured() ? Error::None : Error::NotConfigured, {}, false});
    return id;
}

void Audiobookshelf::withLibrary(RequestId id, const std::string& folder, std::function<void(Error)> fail,
                                 std::function<void(const std::string&)> next) {
    if (!folder.empty()) return next(folder);
    if (!libraryId_.empty()) return next(libraryId_);
    send(
        id, "/api/libraries",
        [](const std::string& body) {
            auto parsed = audiobookshelf::parseLibraries(body);
            return Reply{parsed.error, std::move(parsed.items), false};
        },
        [this, fail = std::move(fail), next = std::move(next)](Reply reply) {
            auto* libraries = std::get_if<std::vector<audiobookshelf::Library>>(&reply.data);
            if (reply.error != Error::None || !libraries) return fail(reply.error == Error::None ? Error::Server : reply.error);
            auto wanted = libraries->end();
            if (config_.library.empty()) { // the first by display order
                if (!libraries->empty()) wanted = libraries->begin();
            } else { // the named one; if it is not there, no other is listed in its place
                wanted = std::find_if(libraries->begin(), libraries->end(), [this](const audiobookshelf::Library& library) {
                    return library.id == config_.library || library.name == config_.library;
                });
            }
            if (wanted == libraries->end()) return fail(Error::Server);
            libraryId_ = wanted->id;
            next(libraryId_);
        });
}

MusicServer::RequestId Audiobookshelf::ping(std::function<void(Error, ServerInfo)> done) {
    RequestId id = ++lastId_;
    send(
        id, "/status",
        [](const std::string& body) {
            auto result = audiobookshelf::parseStatus(body);
            return Reply{result.error, std::move(result.info), false};
        },
        [this, id, done](Reply reply) {
            auto* info = std::get_if<ServerInfo>(&reply.data);
            if (reply.error != Error::None || !info) return done(reply.error == Error::None ? Error::Server : reply.error, {});
            send(
                id, "/api/me", [](const std::string& body) { return Reply{audiobookshelf::parseMe(body), {}, false}; },
                [done, info = std::move(*info)](Reply me) {
                    if (me.error != Error::None) done(me.error, {});
                    else done(Error::None, info);
                });
        });
    return id;
}

MusicServer::RequestId Audiobookshelf::catalogueVersion(std::function<void(Error, std::string)> done) {
    RequestId id = ++lastId_;
    answer(id, [done = std::move(done)](Reply reply) { done(reply.error, {}); },
           Reply{configured() ? Error::None : Error::NotConfigured, {}, false});
    return id;
}

MusicServer::RequestId Audiobookshelf::albums(std::string listType, int offset, int size, Done<RemoteAlbum> done,
                                              std::string musicFolderId) {
    std::string sort;
    if (listType == kNewest) sort = "sort=addedAt&desc=1";
    else if (listType == kAlphabetical) sort = "sort=media.metadata.title";
    else return noItems<RemoteAlbum>(std::move(done));
    const int pageSize = std::clamp(size, 1, kMaxPageSize);
    offset = std::max(offset, 0);
    const std::string query = "/items?limit=" + std::to_string(pageSize) + "&page=" + std::to_string(offset / pageSize) + "&" + sort;
    RequestId id = ++lastId_;
    withLibrary(
        id, musicFolderId, [done](Error error) { done(error, {}, false); },
        [this, id, query, offset, pageSize, done](const std::string& library) {
            sendList<RemoteAlbum>(
                id, "/api/libraries/" + percentEncode(library) + query,
                [offset, pageSize](const std::string& body) { return audiobookshelf::parseBookList(body, offset, pageSize); }, done);
        });
    return id;
}

MusicServer::RequestId Audiobookshelf::musicFolders(Done<RemoteFolder> done) {
    RequestId id = ++lastId_;
    sendList<RemoteFolder>(
        id, "/api/libraries",
        [library = config_.library](const std::string& body) {
            auto parsed = audiobookshelf::parseLibraries(body);
            audiobookshelf::Parsed<RemoteFolder> folders;
            folders.error = parsed.error;
            for (const auto& entry : parsed.items)
                if (library.empty() || entry.id == library || entry.name == library) folders.items.push_back({entry.id, entry.name});
            return folders;
        },
        std::move(done));
    return id;
}

MusicServer::RequestId Audiobookshelf::artists(Done<RemoteArtist> done) {
    RequestId id = ++lastId_;
    withLibrary(
        id, {}, [done](Error error) { done(error, {}, false); },
        [this, id, done](const std::string& library) {
            sendList<RemoteArtist>(id, "/api/libraries/" + percentEncode(library) + "/authors", audiobookshelf::parseAuthors, done);
        });
    return id;
}

MusicServer::RequestId Audiobookshelf::artistAlbums(std::string artistId, Done<RemoteAlbum> done) {
    RequestId id = ++lastId_;
    sendList<RemoteAlbum>(id, "/api/authors/" + percentEncode(artistId) + "?include=items", audiobookshelf::parseAuthorBooks,
                          std::move(done));
    return id;
}

MusicServer::RequestId Audiobookshelf::albumTracks(std::string albumId, Done<RemoteTrack> done) {
    return book(std::move(albumId), [done = std::move(done)](Error error, RemoteBook book) {
        done(error, std::move(book.tracks), false);
    });
}

MusicServer::RequestId Audiobookshelf::book(std::string albumId, std::function<void(Error, RemoteBook)> done) {
    RequestId id = ++lastId_;
    send(
        id, "/api/items/" + percentEncode(albumId) + "?expanded=1",
        [](const std::string& body) {
            auto result = audiobookshelf::parseBook(body);
            return Reply{result.error, std::move(result.book), false};
        },
        [done = std::move(done)](Reply reply) {
            auto* book = std::get_if<RemoteBook>(&reply.data);
            if (reply.error != Error::None || !book) done(reply.error == Error::None ? Error::Server : reply.error, {});
            else done(Error::None, std::move(*book));
        });
    return id;
}

MusicServer::RequestId Audiobookshelf::progress(std::string albumId,
        std::function<void(Error, std::optional<RemoteProgress>)> done) {
    RequestId id = ++lastId_;
    send(id, "/api/me/progress/" + percentEncode(albumId),
         [](const std::string& body) {
             auto result = audiobookshelf::parseProgress(body);
             return Reply{result.error, std::move(result.progress), false};
         },
         [done = std::move(done)](Reply reply) {
             auto* progress = std::get_if<std::optional<RemoteProgress>>(&reply.data);
             if (reply.error != Error::None || !progress)
                 done(reply.error == Error::None ? Error::Server : reply.error, std::nullopt);
             else done(Error::None, std::move(*progress));
         }, std::nullopt, true);
    return id;
}

void Audiobookshelf::saveProgress(std::string albumId, RemoteProgress progress, std::function<void(Error)> done) {
    RequestId id = ++lastId_;
    progressSaveIds_.insert(id);
    auto complete = [this, id, done = std::move(done)](Reply reply) {
        progressSaveIds_.erase(id);
        if (reply.error != Error::None)
            std::fprintf(stderr, "Audiobookshelf: progress save failed (%d)\n", static_cast<int>(reply.error));
        if (done) done(reply.error);
    };
    auto body = audiobookshelf::progressBody(progress);
    if (!body) {
        answer(id, std::move(complete), Reply{configured() ? Error::Server : Error::NotConfigured, {}, false});
        return;
    }
    send(id, "/api/me/progress/" + percentEncode(albumId), [](const std::string&) { return Reply{}; },
         std::move(complete), std::move(body));
}

MusicServer::RequestId Audiobookshelf::inProgress(Done<RemoteAlbum> done) {
    RequestId id = ++lastId_;
    withLibrary(id, {}, [done](Error error) { done(error, {}, false); },
        [this, id, done](const std::string& library) {
            sendList<RemoteAlbum>(id, "/api/me/items-in-progress",
                [library](const std::string& body) { return audiobookshelf::parseInProgress(body, library); }, done);
        });
    return id;
}

MusicServer::RequestId Audiobookshelf::playlists(Done<RemotePlaylist> done) { return noItems<RemotePlaylist>(std::move(done)); }

MusicServer::RequestId Audiobookshelf::playlistTracks(std::string, Done<RemoteTrack> done) {
    RequestId id = ++lastId_; // there are no playlists, so no id of one exists
    answer(id, [done = std::move(done)](Reply reply) { done(reply.error, {}, false); },
           Reply{configured() ? Error::Server : Error::NotConfigured, {}, false});
    return id;
}

MusicServer::RequestId Audiobookshelf::createPlaylist(std::string, std::vector<std::string>,
                                                      std::function<void(Error, std::string)> done) {
    RequestId id = ++lastId_;
    answer(id, [done = std::move(done)](Reply reply) { done(reply.error, {}); },
           Reply{configured() ? Error::Server : Error::NotConfigured, {}, false});
    return id;
}

void Audiobookshelf::cancel(RequestId id) {
    if (waiting_.erase(id)) http_->cancel(id);
}

MusicServer::Stream Audiobookshelf::stream(const std::string& trackId, int) {
    const std::size_t split = trackId.find(kIdSeparator);
    if (!configured() || split == std::string::npos || split == 0 || split + 1 == trackId.size()) return {};
    return {config_.url + "/api/items/" + percentEncode(trackId.substr(0, split)) + "/file/" +
                percentEncode(trackId.substr(split + 1)) + "?token=" + percentEncode(config_.apiKey),
            std::nullopt};
}

void Audiobookshelf::poll() {
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
