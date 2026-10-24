#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
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

// One audio file at a time.
class MediaPlayer {
public:
    virtual ~MediaPlayer() = default;
    virtual bool open(const std::string& path, double startAt = 0) = 0;
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
    // What Player opens. It holds credentials: never speak or log it.
    virtual std::string streamUrl(const std::string& trackId) const = 0;
    virtual void poll() = 0; // UI thread: run the callbacks of finished requests
};

} // namespace oap
