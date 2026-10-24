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
        std::string format; // "raw" or empty: the original file; else a transcode ("mp3", "opus")
        int maxBitrate = 0; // kbps, 0 = no limit
    };
    // The server.* keys of config.yaml.
    static Config configFrom(const Settings& config);

    using SaltSource = std::function<std::string()>;
    static std::string randomSalt();

    // Requests go through `http` (an HttpClient in the program, a fake in tests).
    Subsonic(Config config, std::unique_ptr<Http> http, SaltSource salt = randomSalt);
    explicit Subsonic(Config config);
    ~Subsonic() override;
    Subsonic(const Subsonic&) = delete;
    Subsonic& operator=(const Subsonic&) = delete;

    bool configured() const override;
    RequestId ping(std::function<void(Error, ServerInfo)> done) override;
    RequestId albums(std::string listType, int offset, int size, Done<RemoteAlbum> done) override;
    RequestId artists(Done<RemoteArtist> done) override;
    RequestId artistAlbums(std::string artistId, Done<RemoteAlbum> done) override;
    RequestId albumTracks(std::string albumId, Done<RemoteTrack> done) override;
    RequestId playlists(Done<RemotePlaylist> done) override;
    RequestId playlistTracks(std::string playlistId, Done<RemoteTrack> done) override;
    void cancel(RequestId id) override;
    std::string streamUrl(const std::string& trackId) const override;
    void poll() override;

    static constexpr int kMaxPageSize = 500; // getAlbumList2 returns at most this many

private:
    using Params = std::vector<std::pair<std::string, std::string>>;
    // What the HTTP thread hands to poll(): one of the result types, or just an error.
    struct Reply {
        Error error = Error::None;
        std::variant<std::monostate, ServerInfo, std::vector<RemoteAlbum>, std::vector<RemoteArtist>,
                     std::vector<RemoteTrack>, std::vector<RemotePlaylist>>
            data;
        bool more = false;
    };
    using Parser = std::function<Reply(const std::string& body)>; // runs on the HTTP thread
    struct Inbox {                                                 // shared with the HTTP thread
        std::mutex mutex;
        std::vector<std::pair<RequestId, Reply>> ready;
    };

    std::string url(const std::string& method, const Params& params) const;
    RequestId request(const std::string& method, const Params& params, Parser parse, std::function<void(Reply)> done);
    template <class T, class P> RequestId list(const std::string& method, const Params& params, P parse, Done<T> done);

    Config config_;
    SaltSource salt_;
    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    RequestId lastId_ = 0;
    std::unordered_map<RequestId, std::function<void(Reply)>> waiting_; // UI thread only
    std::unique_ptr<Http> http_; // reset first in the destructor
};

} // namespace oap
