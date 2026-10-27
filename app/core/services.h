#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
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

// Whole text files on disk, for what core keeps outside Settings (the manifests
// of the track cache). Core does no file I/O of its own: app/config/ has the
// real implementation (DiskTextFiles), tests use a real one on a temp folder.
class TextFiles {
public:
    virtual ~TextFiles() = default;
    // The whole file; none if it does not exist or cannot be read.
    virtual std::optional<std::string> read(const std::filesystem::path& file) = 0;
    // Replaces the file all at once: the old contents or the new ones are left
    // after a crash or a power cut, never half of either. Creates the folder.
    // False if it could not be written (the old file is then untouched).
    virtual bool writeAtomically(const std::filesystem::path& file, const std::string& contents) = 0;
};

// One track of a library item. `location` is a file path for a local or cached
// track and the server's track id for a server track.
struct IndexTrack {
    std::string location;
    std::string title;
    std::string artist;
    int number = 0;       // track number, 0: unknown
    double duration = 0;  // seconds
    int64_t size = 0;     // bytes
    int64_t fileTime = 0; // wall time (seconds since 1970) the file last changed
    std::vector<std::string> seenOn; // server references, see LibraryIndex::seenRef()
    bool operator==(const IndexTrack&) const = default;
};

// One entry of the library index: a book, an album or a podcast, wherever it lives.
struct IndexItem {
    enum class Source { Local, Server, Cached };
    enum class Kind { Book, Music, Podcast };
    std::string id;
    Source source = Source::Local;
    Kind kind = Kind::Music;
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    int year = 0;                // 0: unknown
    std::string musicBrainzId;   // the album's MusicBrainz id, may be empty
    int64_t firstSeen = 0;       // wall time the item first entered the index
    int64_t lastPlayed = 0;      // wall time it last played, 0: never
    std::vector<IndexTrack> tracks;
    std::vector<std::string> seenOn; // server references, see LibraryIndex::seenRef()
    bool operator==(const IndexItem&) const = default;
};

// Where the library index is kept between runs. Core holds the index in memory
// and only loads and saves it through this; the real store (SQLite) lives in app/library/.
class LibraryStore {
public:
    virtual ~LibraryStore() = default;
    // Everything stored; empty when there is nothing or it cannot be read.
    virtual std::vector<IndexItem> load() = 0;
    // Writes `changed` (each replaces the stored item with its id) and deletes the items
    // named in `removed`, all or nothing. False on failure: nothing is then stored.
    virtual bool save(const std::vector<IndexItem>& changed, const std::vector<std::string>& removed) = 0;
};

// What the tags of a local audio file say. A text field the file does not have is
// empty and a number it does not have is 0.
struct TrackTags {
    std::string title;
    std::string artist;
    std::string albumArtist;
    std::string album;
    std::string genre;
    std::string musicBrainzAlbumId;
    int track = 0;       // 0: unknown ("3/12" gives 3)
    int disc = 0;        // 0: unknown
    int year = 0;        // 0: unknown
    double duration = 0; // seconds, 0 if unknown
    bool operator==(const TrackTags&) const = default;
};

// Reads the tags and the length of a local audio file (implemented with FFmpeg in
// app/media/). Slow: it opens the file, so callers use it off the UI thread.
class TagReader {
public:
    virtual ~TagReader() = default;
    // None for a file that cannot be read or has no audio. A file without tags gives
    // its duration and empty fields. Implementations must be safe to call from any
    // thread, also from several at once.
    virtual std::optional<TrackTags> read(const std::filesystem::path& file) = 0;
};

// What a scan of the local files starts from.
struct ScanRequest {
    std::filesystem::path libraryRoot; // books: a folder (or a loose file) under it is one book
    std::filesystem::path musicRoot;   // music, grouped by album tags
    std::filesystem::path skip;        // the cache of downloaded tracks: never entered
    // The local items of the last scan. A file listed there whose path, size and file
    // time have not changed is kept as it was and its tags are not read again.
    std::vector<IndexItem> previous;
};

// The prefixes of the ids of local items (the rest is the path, or the tags).
inline constexpr const char* kLocalBookPrefix = "local:book:";
inline constexpr const char* kLocalFolderPrefix = "local:folder:";
inline constexpr const char* kLocalAlbumPrefix = "local:album:";

// Lists the local files as index items, in the background. Like MusicServer it never
// blocks: scan() returns its id at once, and the callback runs later, on the UI thread,
// from poll(). Once cancel(id) is called that scan's callback never runs.
//
// The result is every local item (source Local), complete: items of earlier scans that are
// not in it are gone. Books keep their folder: one Book item per folder (or loose file)
// under libraryRoot, titled by the folder name, with a track per audio file. Music is
// grouped by tags: one Music item per album artist + album (the artist when the file has
// no album artist), and an album's files in different folders are one item; files without
// an album tag are grouped by the folder under musicRoot, titled by its name. An unreadable
// file is listed with its file name as title and no other data. firstSeen and lastPlayed
// are left 0 (LibraryIndex::replaceSource() fills them in). Ids derive from the path or
// the tags, so they are the same in every scan: "local:book:<path>", "local:folder:<path>",
// "local:album:<album artist>\x1f<album>".
class LibraryScanner {
public:
    using RequestId = std::uint64_t; // unique per scanner, never 0
    using Done = std::function<void(std::vector<IndexItem>)>;
    virtual ~LibraryScanner() = default;
    virtual RequestId scan(ScanRequest request, Done done) = 0;
    virtual void cancel(RequestId id) = 0; // its callback will not run; unknown ids are ignored
    virtual void poll() = 0;               // UI thread: run the callbacks of finished scans
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
    // if it is still running, open(path) goes on with it (opening()). A preload
    // is the stream from its start: open(path) of a transcode with an offset URL
    // at a later whole second opens that URL instead, and leaves the preload. One
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
    std::string suffix{}; // file type of the original, e.g. "flac"; empty if unknown
    long long size = 0;   // bytes of the original, 0 if unknown
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

// Fetches tracks of a music server into files, one after the other, in the
// background (Subsonic first, implemented in app/net/). Like MusicServer it
// never blocks: download() queues the track and returns its id at once, and the
// callbacks run later, on the UI thread, from poll(). cancel(id) stops the
// transfer, and the callbacks of that id never run.
//
// The bytes go to `<dest>.part`. A `.part` left by an earlier try (a failure, a
// cancel, a restart) is continued from where it ends, or started over when the
// server cannot continue it. Only a complete file is renamed to `dest`, so
// `dest` exists only when it is whole. A failure or cancel keeps the `.part`.
// Folders above `dest` are created. A `dest` that exists is replaced.
//
// Whole means: the length the server sends (Content-Length, or the total of
// Content-Range for a continued `.part`) is authoritative; a body that ends before
// it is Unreachable. Only when the server sends no length is the file compared
// with `expectedSize` (0: not known, anything is accepted); a shorter body is
// Unreachable too, and the `.part` stays; a longer one is taken (the size was stale). After cancel() returns the downloader
// creates no folder or file for that request any more.
class Downloader {
public:
    enum class Result {
        Ok,
        NotConfigured, // no server to fetch from
        Unreachable,   // no answer, or the connection broke off or ended early: the `.part` can be continued
        Server,        // the server refused (an HTTP error status, or an error instead of audio)
        Disk,          // the file or its folder could not be written
    };
    using RequestId = std::uint64_t; // unique per downloader, never 0
    // Bytes of the file so far (what a continued `.part` already had included)
    // and its whole size, 0 if the server did not say. May be skipped when it
    // comes faster than poll() is called: only the latest is delivered.
    using Progress = std::function<void(long long bytes, long long total)>;
    using Done = std::function<void(Result)>;

    virtual ~Downloader() = default;
    virtual bool configured() const = 0;
    // `progress` may be empty. `done` runs once, unless the request is cancelled.
    virtual RequestId download(const std::string& trackId, const std::string& dest, long long expectedSize,
                               Progress progress, Done done) = 0;
    virtual void cancel(RequestId id) = 0; // unknown ids are ignored
    virtual void poll() = 0;               // UI thread: run the callbacks
};

} // namespace oap
