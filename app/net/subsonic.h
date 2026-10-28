#pragma once

#include "core/services.h"
#include "net/http_client.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace oap {

class Settings;

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
    // The server.* keys of config.yaml.
    static Config configFrom(const Settings& config);
    // The server config.yaml sets up, if it is a Subsonic one: `server.type` is
    // "subsonic" (in any case) or left out, and there is a `server.url`. Else nullptr, and the
    // front end uses a server that is not configured.
    static bool handles(const Settings& config); // fromConfig() would make a server
    static std::unique_ptr<Subsonic> fromConfig(const Settings& config);

    using SaltSource = std::function<std::string()>;
    static std::string randomSalt();

    // The URL of a track's original file for the Downloader (`download?id=`),
    // with the same token auth as stream(). It holds credentials: never speak
    // or log it. Each call has a fresh salt. Empty when `config` has no url.
    using DownloadUrls = std::function<std::string(const std::string& trackId)>;
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
    RequestId playlists(Done<RemotePlaylist> done) override;
    RequestId playlistTracks(std::string playlistId, Done<RemoteTrack> done) override;
    // `createPlaylist` with `name` and one `songId` per track, a GET like every call (HttpClient has no POST).
    RequestId createPlaylist(std::string name, std::vector<std::string> trackIds, std::function<void(Error)> done) override;
    void cancel(RequestId id) override;
    // A transcode is MP3: every Subsonic server can make it, and every bit rate
    // of the Streaming quality setting is a standard MP3 rate. It seeks with
    // timeOffset when the server has the OpenSubsonic extension transcodeOffset.
    // The first transcode asks the server for its extensions (and the next one
    // again if no answer came, or an error, up to three errors: a server that is
    // not OpenSubsonic); until the answer is in, a transcode gets no offset URL,
    // so it seeks by opening it again from the start.
    Stream stream(const std::string& trackId, int maxBitRate) override;
    // A failure is only logged to stderr, by its kind: never the URL.
    void scrobble(const std::string& trackId, bool submission) override;
    void poll() override;

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
    enum class Known { NotYet, Asking, Yes, No };
    Known transcodeOffset_ = Known::NotYet; // whether a transcode can start later (timeOffset)
    int extensionErrors_ = 0;               // answers to getOpenSubsonicExtensions that were errors
    std::unique_ptr<Http> http_; // reset first in the destructor
};

} // namespace oap
