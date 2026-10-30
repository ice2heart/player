#pragma once

#include "core/services.h"
#include "net/http_client.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace oap {

// A Subsonic server (Navidrome, Gonic, Airsonic, ...), API 1.16.1 with token
// auth: each request sends u, t = md5(password + salt) and a fresh random salt s,
// never the password. Answers are parsed on the HTTP thread; the callbacks run
// on the UI thread from poll(). URLs hold the user name and a token that does
// not expire, so they are never logged, printed or put in an error.
class Subsonic : public MusicServer {
public:
    struct Config {
        std::string url, user, password;
    };
    // The url, user and password of a Subsonic entry of config.yaml.
    static Config configFrom(const ServerConfig& server);

    using SaltSource = std::function<std::string()>;
    static std::string randomSalt();

    // The URL for the Downloader: original (`download?id=`) at bitrate 0, else
    // an MP3 transcode (`stream`, `format=mp3`, `maxBitRate` in kbps),
    // with the same token auth as stream(). It holds credentials: never speak
    // or log it. Each call has a fresh salt. Empty when `config` has no url.
    using DownloadUrls = std::function<std::string(const std::string& trackId, int bitrate)>;
    static DownloadUrls downloadUrls(Config config, SaltSource salt = randomSalt);

    // Requests go through `http` (an HttpClient in the program, a fake in tests).
    Subsonic(Config config, std::unique_ptr<Http> http, SaltSource salt = randomSalt);
    explicit Subsonic(Config config);
    ~Subsonic() override;
    Subsonic(const Subsonic&) = delete;
    Subsonic& operator=(const Subsonic&) = delete;

    bool configured() const override;
    RequestId ping(std::function<void(Error, ServerInfo)> done) override;
    RequestId catalogueVersion(std::function<void(Error, std::string)> done) override;
    RequestId albums(std::string listType, int offset, int size, Done<RemoteAlbum> done,
                     std::string musicFolderId = {}) override;
    RequestId musicFolders(Done<RemoteFolder> done) override;
    RequestId artists(Done<RemoteArtist> done) override;
    RequestId artistAlbums(std::string artistId, Done<RemoteAlbum> done) override;
    RequestId albumTracks(std::string albumId, Done<RemoteTrack> done) override;
    // getAlbum; a Subsonic album has no chapters (its tracks are the parts).
    RequestId book(std::string albumId, std::function<void(Error, RemoteBook)> done) override;
    Catalogue catalogue() const override { return Catalogue::Music; }
    bool syncsProgress() const override { return false; }
    RequestId progress(std::string albumId, std::function<void(Error, std::optional<RemoteProgress>)> done) override;
    void saveProgress(std::string albumId, RemoteProgress progress, std::function<void(Error)> done = {}) override;
    RequestId inProgress(Done<RemoteAlbum> done) override;
    bool supportsStars() const override { return true; }
    RequestId trackDetails(std::string trackId, Done<RemoteTrack> done) override;
    RequestId starredTracks(Done<RemoteTrack> done) override;
    RequestId setStarred(std::string trackId, bool starred, std::function<void(Error)> done) override;
    RequestId playlists(Done<RemotePlaylist> done) override;
    RequestId playlistTracks(std::string playlistId, Done<RemoteTrack> done) override;
    // `createPlaylist` with `name` and one `songId` per track, a GET like every call (HttpClient has no POST).
    RequestId createPlaylist(std::string name, std::vector<std::string> trackIds, std::function<void(Error, std::string)> done) override;
    void cancel(RequestId id) override;
    // A transcode is MP3: every Subsonic server can make it, and every bit rate
    // of the Streaming quality setting is a standard MP3 rate. It seeks with
    // timeOffset when the server has the OpenSubsonic extension transcodeOffset.
    // The first transcode asks the server for its extensions (and the next one
    // again if no answer came, or an error, up to three errors: a server that is
    // not OpenSubsonic); until the answer is in, a transcode gets no offset URL,
    // so it seeks by opening it again from the start.
    Stream stream(const std::string& trackId, int maxBitRate) override;
    // `time` is sent when `timeMs` is set. A failure is also logged to stderr, by its kind: never the URL.
    void scrobble(const std::string& trackId, bool submission, std::optional<std::int64_t> timeMs,
                  std::function<void(Error)> done) override;
    void poll() override;
    // Before the program exits: HttpClient::finish(). The answers to scrobbles that come in meanwhile
    // are delivered to their callbacks (the UI thread, no poll() follows); no other callback runs.
    // A request after it is not sent.
    void finish(std::chrono::milliseconds deadline);

    static constexpr int kMaxPageSize = 500; // getAlbumList2 returns at most this many
    static constexpr const char* kTranscodeFormat = "mp3";

private:
    using Params = std::vector<std::pair<std::string, std::string>>;
    // What the HTTP thread hands to poll(): one of the result types, or just an error.
    struct Reply {
        Error error = Error::None;
        std::variant<std::monostate, ServerInfo, std::vector<RemoteAlbum>, std::vector<RemoteArtist>,
                     std::vector<RemoteTrack>, std::vector<RemotePlaylist>, std::vector<RemoteFolder>,
                     std::vector<std::string>, std::string>
            data;
        bool more = false;
    };
    using Parser = std::function<Reply(const std::string& body)>; // runs on the HTTP thread
    struct Inbox {                                                 // shared with the HTTP thread
        std::mutex mutex;
        std::vector<std::pair<RequestId, Reply>> ready;
    };

    static std::string buildUrl(const Config& config, const std::string& salt, const std::string& method,
                                const Params& params);
    std::string url(const std::string& method, const Params& params) const;
    RequestId request(const std::string& method, const Params& params, Parser parse, std::function<void(Reply)> done);
    template <class T, class P> RequestId list(const std::string& method, const Params& params, P parse, Done<T> done);
    void askExtensions(); // getOpenSubsonicExtensions, for transcodeOffset_

    Config config_;
    SaltSource salt_;
    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    RequestId lastId_ = 0;
    std::unordered_map<RequestId, std::function<void(Reply)>> waiting_; // UI thread only
    std::set<RequestId> scrobbleIds_;                                   // scrobbles waiting: finish() answers them
    enum class Known { NotYet, Asking, Yes, No };
    Known transcodeOffset_ = Known::NotYet; // whether a transcode can start later (timeOffset)
    int extensionErrors_ = 0;               // answers to getOpenSubsonicExtensions that were errors
    std::unique_ptr<Http> http_; // reset first in the destructor
};

} // namespace oap
