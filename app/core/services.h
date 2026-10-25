#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace oap {

// Everything App needs from the outside world. The real implementations live in
// app/media/ (FFmpeg, eSpeak NG) and app/core/ (Earcons, Mixer); tests use fakes.

class Clock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    virtual ~Clock() = default;
    virtual TimePoint now() const = 0;   // monotonic: timers, long presses
    virtual std::time_t wallTime() const = 0; // spoken time of day
};

class SystemClock : public Clock {
public:
    TimePoint now() const override { return std::chrono::steady_clock::now(); }
    std::time_t wallTime() const override { return std::time(nullptr); }
};

// Text to speech. stop() cancels both queued and currently playing speech.
class Speech {
public:
    virtual ~Speech() = default;
    virtual void say(const std::string& text) = 0;
    virtual void stop() = 0;
    virtual bool speaking() const = 0;
    virtual void setRate(int wordsPerMinute) = 0;
};

struct Chapter {
    std::string title;
    double start = 0;
};

// A stream that cannot seek in itself: a transcode, which the server makes
// while it sends it, without byte ranges and often without a length. To go to
// another place (a seek, a retry after a dropout, an open at a position) the
// player opens the stream again.
struct Transcoded {
    // This plus whole seconds is the URL of the stream from that time on. Empty
    // when the server cannot start a transcode later: then the stream opens from
    // its start, and the audio before the place is decoded and dropped. It holds
    // credentials like the stream URL: never speak or log it.
    std::string offsetUrl;
    double duration = 0; // seconds, as the server lists the track; 0 if unknown
};

// One audio file at a time.
class MediaPlayer {
public:
    virtual ~MediaPlayer() = default;
    // Returns false if the open failed at once. A local file is open when this
    // returns true. A stream (a URL) may still be opening then: it opens in the
    // background, opening() stays true until it is done, and then isOpen() says
    // whether it worked. A newer open() or close() drops an open still running,
    // so its result is never seen. Nothing plays until play() after the open.
    // `transcoded` is set for a stream that cannot seek in itself: it seeks by
    // opening the stream again, and its duration() is the one given there.
    virtual bool open(const std::string& path, double startAt = 0,
                      std::optional<Transcoded> transcoded = std::nullopt) = 0;
    virtual bool opening() const = 0;
    // Read-ahead: starts opening the stream `path` in the background, next to
    // what is open and without disturbing it, so that a later open(path) takes
    // it over and need not wait. A local path, or the path already preloaded,
    // does nothing. If the preload failed, open(path) opens it again as usual;
    // if it is still running, open(path) goes on with it (opening()). One
    // preload is kept until open(path) takes it over, a preload of another path
    // replaces it, or cancelPreload() drops it: an open() of another path and
    // close() leave it alone.
    virtual void preload(const std::string& path) = 0;
    virtual void cancelPreload() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual bool playing() const = 0;

    virtual void seek(double seconds) = 0;
    virtual double position() const = 0;
    virtual double duration() const = 0;
    virtual const std::vector<Chapter>& chapters() const = 0; // embedded chapters, may be empty

    // End of file reached while playing and everything has been heard.
    virtual bool finished() const = 0;
    // A stream broke off before its end (the network or the server went away)
    // and everything that came has been heard. finished() stays false then.
    // Local files never fail this way: a read error there ends the file.
    // Before it fails, a stream that loses its connection is retried for a while
    // (the place is kept), and buffering() is true whenever it has run dry meanwhile.
    virtual bool failed() const = 0;
    // A stream that plays has run dry: everything that came has been heard and
    // more is awaited (a slow network, or a dropped connection being retried).
    // False when paused, while opening, and once it has finished or failed.
    virtual bool buffering() const = 0;
};

// Short UI sounds.
enum class Earcon {
    Move,    // moved to another menu item
    Wrap,    // list wrapped around
    Edge,    // nothing there / end of list
    Open,    // entered a submenu
    Close,   // left a submenu
    Error,
    Volume,
    Play,
    Boot,
    Sleep,
    Wake,
    Buffering, // a stream that plays has run dry: waiting for the network
};

class SoundEffects {
public:
    virtual ~SoundEffects() = default;
    virtual void play(Earcon earcon) = 0;
};

class VolumeControl {
public:
    virtual ~VolumeControl() = default;
    virtual void setVolume(float volume) = 0; // 0..1
};

// What a music server lists. Nothing here is specific to one kind of server.
struct RemoteTrack {
    std::string id, title, artist, album;
    double duration = 0; // seconds
    int track = 0;       // number on the album, 0 if unknown
};
struct RemoteAlbum {
    std::string id, name, artist;
    int songCount = 0;
};
struct RemoteArtist {
    std::string id, name;
    int albumCount = 0;
};
struct RemotePlaylist {
    std::string id, name;
    int songCount = 0;
};

// A server to browse and stream from (Subsonic first, implemented in app/net/).
// Requests never block: each one is queued and returns its id at once. The
// callback runs later, on the UI thread, from poll(). Once cancel(id) is called
// that request's callback never runs, so a late answer for a screen that is gone
// is dropped. Callbacks must not hold pointers to screens; keep the id instead.
class MusicServer {
public:
    enum class Error { None, NotConfigured, Unreachable, BadLogin, Server };
    struct ServerInfo {
        std::string type, version; // "navidrome", "0.64.2"
    };
    using RequestId = std::uint64_t; // unique per server object, never 0
    // What the player opens for a track. Its URLs hold credentials: never speak or log them.
    struct Stream {
        std::string url;
        std::optional<Transcoded> transcoded; // set for a transcode; its duration is left 0
    };
    // `more` is true when the server may have another page after this one.
    // On an error the list is empty and `more` is false. When the server is not
    // configured, every request finishes (through poll()) with NotConfigured.
    template <class T> using Done = std::function<void(Error, std::vector<T>, bool more)>;

    virtual ~MusicServer() = default;
    virtual bool configured() const = 0;
    virtual RequestId ping(std::function<void(Error, ServerInfo)> done) = 0;
    virtual RequestId albums(std::string listType, int offset, int size, Done<RemoteAlbum> done) = 0;
    virtual RequestId artists(Done<RemoteArtist> done) = 0;
    virtual RequestId artistAlbums(std::string artistId, Done<RemoteAlbum> done) = 0;
    virtual RequestId albumTracks(std::string albumId, Done<RemoteTrack> done) = 0;
    virtual RequestId playlists(Done<RemotePlaylist> done) = 0;
    virtual RequestId playlistTracks(std::string playlistId, Done<RemoteTrack> done) = 0;
    virtual void cancel(RequestId id) = 0; // its callback will not run; unknown ids are ignored
    // The stream of a track: the original file with `maxBitRate` 0, else a
    // transcode to at most that many kbit/s. Empty URLs when not configured.
    virtual Stream stream(const std::string& trackId, int maxBitRate) = 0;
    // Tells the server a track plays now (`submission` false: "now playing")
    // or has been heard (true: play count, "Recently played"). Sent in the
    // background like any request, but nothing comes back: whether it worked
    // is not reported, so a failed scrobble is never heard. Not configured: nothing.
    virtual void scrobble(const std::string& trackId, bool submission) = 0;
    virtual void poll() = 0; // UI thread: run the callbacks of finished requests
};

} // namespace oap
