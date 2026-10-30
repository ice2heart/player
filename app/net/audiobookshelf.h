#pragma once

#include "core/services.h"
#include "net/audiobookshelf_json.h"
#include "net/http_client.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace oap {

// An Audiobookshelf server (2.26 or later: API keys), as a MusicServer of books: a book is a
// RemoteAlbum, an audio file a RemoteTrack (id "<item id>/<ino>"), an author a RemoteArtist and
// the chosen book library the only RemoteFolder. Every request carries `Authorization: Bearer
// <api key>`; the key is in no URL but the one stream() returns, which is for the player and
// the downloader only: never speak, log or print it. Answers are parsed on the HTTP thread;
// the callbacks run on the UI thread from poll().
class Audiobookshelf : public MusicServer {
public:
    struct Config {
        std::string url, user, apiKey;
        std::string library; // name or id of the book library to use; empty: the first by display order
    };
    // The url, user, api key and library of an Audiobookshelf entry of config.yaml.
    static Config configFrom(const ServerConfig& server);

    // Requests go through `http` (an HttpClient in the program, a fake in tests).
    Audiobookshelf(Config config, std::unique_ptr<Http> http);
    explicit Audiobookshelf(Config config);
    ~Audiobookshelf() override;
    Audiobookshelf(const Audiobookshelf&) = delete;
    Audiobookshelf& operator=(const Audiobookshelf&) = delete;

    bool configured() const override;
    RequestId playQueue(std::function<void(Error, std::optional<RemoteQueue>)> done) override;
    RequestId savePlayQueue(RemoteQueue queue, std::function<void(Error)> done) override;
    // GET /status (type "Audiobookshelf", `serverVersion`), then GET /api/me (401: BadLogin).
    RequestId ping(std::function<void(Error, ServerInfo)> done) override;
    // There is no cheap "changed?" check: the version is always empty, so a sync always fetches.
    RequestId catalogueVersion(std::function<void(Error, std::string)> done) override;
    // "newest" (sorted by date added, latest first) and "alphabeticalByName" (by title); any other
    // list type is empty. `offset` / `size` become page = offset / size; `size` is at most kMaxPageSize.
    // `musicFolderId`: a library id; empty: the configured library, else the first book library.
    RequestId albums(std::string listType, int offset, int size, Done<RemoteAlbum> done,
                     std::string musicFolderId = {}) override;
    // The book libraries (never a podcast library), or only the one named by Config::library.
    RequestId musicFolders(Done<RemoteFolder> done) override;
    RequestId artists(Done<RemoteArtist> done) override;       // the authors of the library
    RequestId artistAlbums(std::string artistId, Done<RemoteAlbum> done) override;
    RequestId albumTracks(std::string albumId, Done<RemoteTrack> done) override;
    // GET /api/items/<id>?expanded=1
    RequestId book(std::string albumId, std::function<void(Error, RemoteBook)> done) override;
    Catalogue catalogue() const override { return Catalogue::Books; }
    bool syncsProgress() const override { return true; }
    RequestId progress(std::string albumId, std::function<void(Error, std::optional<RemoteProgress>)> done) override;
    void saveProgress(std::string albumId, RemoteProgress progress, std::function<void(Error)> done = {}) override;
    // No playlists: an empty list, and creating one is Error::Server.
    RequestId inProgress(Done<RemoteAlbum> done) override;
    bool supportsStars() const override { return false; }
    RequestId trackDetails(std::string trackId, Done<RemoteTrack> done) override;
    RequestId starredTracks(Done<RemoteTrack> done) override;
    RequestId setStarred(std::string trackId, bool starred, std::function<void(Error)> done) override;
    RequestId playlists(Done<RemotePlaylist> done) override;
    RequestId playlistTracks(std::string playlistId, Done<RemoteTrack> done) override;
    RequestId createPlaylist(std::string name, std::vector<std::string> trackIds, std::function<void(Error, std::string)> done) override;
    RequestId addPlaylistTrack(std::string playlistId, std::string trackId, std::function<void(Error)> done) override;
    RequestId removePlaylistTrack(std::string playlistId, std::size_t index, std::function<void(Error)> done) override;
    RequestId deletePlaylist(std::string playlistId, std::function<void(Error)> done) override;
    void cancel(RequestId id) override;
    // `<url>/api/items/<item>/file/<ino>?token=<api key>`: always the original file (it seeks
    // with Range), `maxBitRate` does not apply. Empty when not configured or the id is not "<item>/<ino>".
    Stream stream(const std::string& trackId, int maxBitRate) override;
    void scrobble(const std::string&, bool, std::optional<std::int64_t>, std::function<void(Error)>) override {}
    void poll() override;

    // Exit: transport drains can run in parallel. finish() delivers only save
    // acknowledgements, on its caller, after the worker has stopped.
    void finishTransport(std::chrono::milliseconds deadline);
    void finish(std::chrono::milliseconds deadline);

    static constexpr int kMaxPageSize = 500;

private:
    struct Reply {
        Error error = Error::None;
        std::variant<std::monostate, ServerInfo, std::vector<RemoteAlbum>, std::vector<RemoteArtist>,
                     std::vector<RemoteFolder>, std::vector<audiobookshelf::Library>, RemoteBook, std::optional<RemoteProgress>>
            data;
        bool more = false;
    };
    using Parser = std::function<Reply(const std::string& body)>; // runs on the HTTP thread
    struct Inbox {                                                 // shared with the HTTP thread
        std::mutex mutex;
        std::vector<std::pair<RequestId, Reply>> ready;
    };

    // Sends GET `path`, or PATCH with patchBody: `done` waits in waiting_. A request that goes
    // in two steps (ping, a list of the default library) sends the second under the same id.
    void send(RequestId id, const std::string& path, Parser parse, std::function<void(Reply)> done,
              std::optional<std::string> patchBody = std::nullopt, bool missingProgressIsNone = false);
    // Answers request `id` from poll() with no HTTP request.
    void answer(RequestId id, std::function<void(Reply)> done, Reply reply);
    template <class T, class P> void sendList(RequestId id, const std::string& path, P parse, Done<T> done);
    // Calls `next` with the id of the library to use, asking the server first when `folder` is empty
    // and it is not known yet; `fail` gets the error when there is none.
    void withLibrary(RequestId id, const std::string& folder, std::function<void(Error)> fail,
                     std::function<void(const std::string& libraryId)> next);
    // A list with nothing in it, answered from poll().
    template <class T> RequestId noItems(Done<T> done);

    Config config_;
    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    RequestId lastId_ = 0;
    std::unordered_map<RequestId, std::function<void(Reply)>> waiting_; // UI thread only
    std::set<RequestId> progressSaveIds_;
    bool transportFinished_ = false;
    std::string libraryId_;                                             // the library to use, once known (UI thread only)
    std::unique_ptr<Http> http_; // reset first in the destructor
};

} // namespace oap
