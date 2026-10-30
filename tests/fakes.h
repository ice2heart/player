#pragma once

// Test doubles for the interfaces in core/services.h. They record what App asked
// for and let the test set what App will observe (time, playback position, ...).

#include "config/disk_text_files.h"
#include "core/services.h"
#include "core/settings.h"
#include "core/track_cache.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oap::test {

// Manifests go to real files in the test's temp folder, written the way the device writes them.
inline TextFiles& diskFiles() {
    static DiskTextFiles files;
    return files;
}

class FakeClock : public Clock {
public:
    TimePoint now() const override { return now_; }
    std::time_t wallTime() const override { return wall_; }
    void advance(std::chrono::milliseconds ms) { now_ += ms; }
    void setWallTime(std::time_t t) { wall_ = t; } // wall time only moves when a test says so

private:
    std::time_t wall_ = 1'700'000'000;
    TimePoint now_{std::chrono::hours(1000)};
};

class FakeSpeech : public Speech {
public:
    void say(const std::string& text) override { said.push_back(text); }
    void stop() override { ++stops; }
    bool speaking() const override { return isSpeaking; }
    void setRate(int wordsPerMinute) override { rate = wordsPerMinute; }

    std::string last() const { return said.empty() ? std::string() : said.back(); }
    bool heard(const std::string& text) const { return std::find(said.begin(), said.end(), text) != said.end(); }

    std::vector<std::string> said;
    int stops = 0;
    int rate = 0;
    bool isSpeaking = false;
};

// With holdStreams set, a stream (a path with "://") opens like on the real
// Player: open() returns true at once, opening() is true, and the open is done
// only when the test calls finishOpen(), which then checks openSucceeds and
// unopenable. Local files and, without holdStreams, streams open at once.
//
//   player.holdStreams = true;
//   app->playRemote("Weather", tracks, 0); // "Loading."; App waits for the open
//   player.finishOpen();                   // it worked (or failed: unopenable)
//   app->tick();                           // App sees the result
//
// preload() of a stream records it in `preloaded` (and `preloads`). An open()
// of that path then takes it over: it is open at once, even with holdStreams,
// unless preloadSucceeds was false when it was asked for (the preload failed),
// and then it opens like any other stream. As on the real Player, a preload is
// the stream from its start: a transcode with an offset URL opened at a later
// second does not take it, and leaves it. A preload of another path replaces it
// and cancelPreload() drops it; an open() of another path and close() leave it.
// `preloads` records every call, local paths too (which the player ignores).
//
// An open with `transcoded` records it; like the real Player, duration() is then
// the one given there, not `length`.
class FakePlayer : public MediaPlayer {
public:
    bool open(const std::string& path, double startAt, std::optional<Transcoded> how) override {
        if (beforeOpen) beforeOpen(path);
        opened.push_back(path);
        bool fromStart = !how || how->offsetUrl.empty() || startAt < 1; // the whole second is 0
        transcoded = std::move(how);
        bool taken = preloaded == path && fromStart;
        bool ready = taken && preloadWorked_;
        if (taken) preloaded.reset();
        if (ready) ++adopted;
        file = path;
        playing_ = false;
        pos = startAt;
        atEnd = false;
        broken = false;
        dry = false;
        opening_ = !ready && holdStreams && isStream(path); // a newer open drops the last one
        isOpen_ = ready || (!opening_ && opens(path)); // what a preload opened stays open
        return opening_ || isOpen_;
    }
    bool opening() const override { return opening_; }
    void preload(const std::string& path) override {
        if (preloaded == path) return;
        preloads.push_back(path);
        if (!isStream(path)) return;
        preloaded = path;
        preloadWorked_ = preloadSucceeds && opens(path);
    }
    void cancelPreload() override { preloaded.reset(); }
    // Ends the pending open: it works unless the path is unopenable. Returns
    // false, and does nothing, when no open is pending (dropped by close() or a
    // newer open, or already done): that is how a late result is simulated.
    bool finishOpen() {
        if (!opening_) return false;
        opening_ = false;
        isOpen_ = opens(file);
        return true;
    }
    void close() override {
        isOpen_ = false;
        opening_ = false;
        playing_ = false;
    }
    bool isOpen() const override { return isOpen_; }
    void play() override { playing_ = isOpen_; } // like the real Player: nothing to play until open
    void pause() override { playing_ = false; }
    bool playing() const override { return playing_; }
    void seek(double seconds) override {
        seeks.push_back(seconds);
        atEnd = false; // a new request makes the real Player unfinished until decoded
        if (!deferSeeks) pos = seconds;
    }
    // Like the real Player, a closed player is at 0 and has no length.
    double position() const override { return isOpen_ ? pos : 0; }
    double duration() const override { return !isOpen_ ? 0 : transcoded ? transcoded->duration : length; }
    const std::vector<Chapter>& chapters() const override { return embedded; }
    bool finished() const override { return isOpen_ && playing_ && atEnd && !broken; }
    bool failed() const override { return isOpen_ && playing_ && broken; }
    // Like the real Player: only while playing, and not once it has finished or failed.
    bool buffering() const override { return isOpen_ && playing_ && dry && !atEnd && !broken; }

    std::vector<std::string> opened;
    std::string file;
    std::optional<Transcoded> transcoded; // of the last open
    std::vector<double> seeks;
    bool deferSeeks = false; // decoder position can lag repeated requests
    double pos = 0;
    double length = 600;
    std::vector<Chapter> embedded;
    bool atEnd = false;
    bool broken = false; // the stream broke off: failed() once playing
    bool dry = false;    // the stream has run dry and is retried: buffering() while playing
    bool openSucceeds = true;
    std::vector<std::string> unopenable; // paths that fail to open
    std::function<void(const std::string& path)> beforeOpen; // e.g. to see what was said by then
    bool holdStreams = false; // streams open only on finishOpen()
    std::optional<std::string> preloaded; // the preload kept now
    std::vector<std::string> preloads;    // every preload() asked for (but the same path again), in order
    int adopted = 0;                      // opens that took a preload over
    bool preloadSucceeds = true;          // false: preloads fail, and their open waits like any other

private:
    static bool isStream(const std::string& path) { return path.find("://") != std::string::npos; }
    bool opens(const std::string& path) const {
        return openSucceeds && std::find(unopenable.begin(), unopenable.end(), path) == unopenable.end();
    }

    bool isOpen_ = false;
    bool opening_ = false;
    bool playing_ = false;
    bool preloadWorked_ = false;
};

class FakeSounds : public SoundEffects {
public:
    void play(Earcon earcon) override { played.push_back(earcon); }
    std::optional<Earcon> last() const { return played.empty() ? std::nullopt : std::optional(played.back()); }
    std::vector<Earcon> played;
};

class FakeVolume : public VolumeControl {
public:
    void setVolume(float v) override { volume = v; }
    float volume = -1;
};

// A settings file in memory: what was last saved, and how often.
class FakeStore : public SettingsStore {
public:
    SettingsMap load() override { return file; }
    bool save(const SettingsMap& values) override {
        if (failSaves) return false; // e.g. the card is read-only for a moment
        file = values;
        ++saves;
        return true;
    }
    std::string onDisk(const std::string& key) const {
        auto it = file.find(key);
        return it == file.end() ? std::string() : it->second;
    }

    SettingsMap file;
    int saves = 0;
    bool failSaves = false;
};

// A library store in memory: what was last saved, and how often.
class FakeLibraryStore : public LibraryStore {
public:
    std::vector<IndexItem> load() override {
        std::vector<IndexItem> out;
        for (const auto& [id, item] : items) out.push_back(item);
        return out;
    }
    bool save(const std::vector<IndexItem>& changed, const std::vector<std::string>& removed) override {
        if (failSaves || failItemSaves) return false;
        ++saves;
        for (const auto& item : changed) {
            items[item.id] = item;
            savedIds.push_back(item.id);
        }
        for (const auto& id : removed) {
            items.erase(id);
            removedIds.push_back(id);
        }
        return true;
    }
    std::vector<IndexPlaylist> loadPlaylists() override { return playlists; }
    bool savePlaylists(const std::vector<IndexPlaylist>& all) override {
        if (failSaves || failPlaylistSaves) return false;
        ++playlistSaves;
        playlists = all;
        return true;
    }

    std::map<std::string, IndexItem> items;
    std::vector<IndexPlaylist> playlists; // what was last saved
    int playlistSaves = 0;
    bool failItemSaves = false;     // only save() fails
    bool failPlaylistSaves = false; // only savePlaylists() fails
    int saves = 0;
    std::vector<std::string> savedIds;   // every item written, in order
    std::vector<std::string> removedIds; // every item deleted, in order
    bool failSaves = false;
};

// Tags by path. A path that is not in `tags` cannot be read. Counts reads per path.
// Thread-safe, as TagReader says, so a test may read from a worker thread.
class FakeTagReader : public TagReader {
public:
    std::optional<TrackTags> read(const std::filesystem::path& file) override {
        std::lock_guard lock(mutex_);
        ++reads_[file];
        auto it = tags.find(file);
        if (it == tags.end()) return std::nullopt;
        return it->second;
    }
    // How many times `file` was read, found or not.
    int reads(const std::filesystem::path& file) const {
        std::lock_guard lock(mutex_);
        auto it = reads_.find(file);
        return it == reads_.end() ? 0 : it->second;
    }
    int totalReads() const {
        std::lock_guard lock(mutex_);
        int total = 0;
        for (const auto& [path, n] : reads_) total += n;
        return total;
    }

    std::map<std::filesystem::path, TrackTags> tags; // set before reading starts

private:
    mutable std::mutex mutex_;
    std::map<std::filesystem::path, int> reads_;
};

// A scanner that delivers only when the test says so. Every scan() is recorded in
// `requests` and stays pending until finish(); the callback then runs on the next
// poll(). cancel() drops a scan whether or not it has finished.
class FakeLibraryScanner : public LibraryScanner {
public:
    struct Request {
        RequestId id = 0;
        ScanRequest request;
    };

    RequestId scan(ScanRequest request, Done done) override {
        RequestId id = ++lastId_;
        requests.push_back({id, std::move(request)});
        pending_.emplace(id, std::move(done));
        return id;
    }
    void cancel(RequestId id) override {
        cancelled.push_back(id);
        pending_.erase(id);
        std::erase_if(ready_, [id](const auto& r) { return r.first == id; });
    }
    void poll() override {
        auto batch = std::move(ready_);
        ready_.clear();
        for (auto& [id, run] : batch) run();
    }

    // Finish a scan with `items`; the callback runs on the next poll(). False if the id is
    // not pending (cancelled, finished, or never made): that is how a late answer is simulated.
    bool finish(RequestId id, std::vector<IndexItem> items) {
        auto it = pending_.find(id);
        if (it == pending_.end()) return false;
        ready_.emplace_back(id, [done = std::move(it->second), items = std::move(items)]() mutable {
            if (done) done(std::move(items));
        });
        pending_.erase(it);
        return true;
    }

    const Request& lastRequest() const {
        if (requests.empty()) throw std::logic_error("FakeLibraryScanner: no scan requested");
        return requests.back();
    }
    std::size_t pendingCount() const { return pending_.size(); }

    std::vector<Request> requests;    // every scan(), in order
    std::vector<RequestId> cancelled; // every cancel() call, in order

private:
    RequestId lastId_ = 0;
    std::map<RequestId, Done> pending_;
    std::vector<std::pair<RequestId, std::function<void()>>> ready_;
};

// The list of one server App gets from a front end whose config.yaml sets up the first server of `settings` (none:
// an entry with no url, as a front end with no server would not make one, but tests still pass their fakes).
inline std::vector<ServerLink> linkTo(const Settings& settings, MusicServer& server, Downloader& downloader) {
    std::vector<ServerConfig> configs = serverConfigs(settings);
    ServerConfig config = configs.empty() ? ServerConfig{} : configs.front();
    std::string key = TrackCache::serverKey(config.url, config.user);
    return {ServerLink{config, key, config.name.empty() ? "Server" : config.name, server, downloader}};
}

// A music server that answers only when the test says so. Every request is
// recorded and stays pending until the test calls reply() / replyPing() / fail().
// Even then the callback runs only on the next poll(), as on the real server, so
// a test can check the "Loading" state in between. cancel() drops a request
// whether or not it has been answered. With isConfigured = false every request
// is answered at once with NotConfigured (still delivered by poll()).
//
//   auto id = server.lastRequest().id;          // what App just asked for
//   server.reply(id, {album1, album2}, /*more=*/false);
//   app.tick();                                 // App calls poll(), callback runs
//
// scrobble() is recorded in `scrobbles`, in order, makes no request, and is answered by the next poll()
// with `scrobbleError` (None: it worked). With `holdScrobbles` the answers wait for answerScrobble().
// A fake that is not configured does not record a scrobble (like Subsonic).
class FakeMusicServer : public MusicServer {
public:
    enum class Kind { Ping, CatalogueVersion, Albums, MusicFolders, Artists, ArtistAlbums, AlbumTracks, Book, Progress, InProgress, Playlists, PlaylistTracks, CreatePlaylist };
    struct Request {
        RequestId id = 0;
        Kind kind = Kind::Ping;
        std::string arg; // listType, artist, album or playlist id, the name of a new playlist; empty otherwise
        int offset = 0;  // Albums only
        int size = 0;    // Albums only
        std::string folder; // Albums only: the musicFolderId, empty for all
        std::vector<std::string> trackIds; // CreatePlaylist only
    };

    bool configured() const override { return isConfigured; }
    RequestId ping(std::function<void(Error, ServerInfo)> done) override { return add(std::move(done), Kind::Ping); }
    RequestId catalogueVersion(std::function<void(Error, std::string)> done) override {
        return add(std::move(done), Kind::CatalogueVersion);
    }
    RequestId albums(std::string listType, int offset, int size, Done<RemoteAlbum> done,
                     std::string musicFolderId = {}) override {
        return add(std::move(done), Kind::Albums, std::move(listType), offset, size, std::move(musicFolderId));
    }
    RequestId musicFolders(Done<RemoteFolder> done) override { return add(std::move(done), Kind::MusicFolders); }
    RequestId artists(Done<RemoteArtist> done) override { return add(std::move(done), Kind::Artists); }
    RequestId artistAlbums(std::string artistId, Done<RemoteAlbum> done) override {
        return add(std::move(done), Kind::ArtistAlbums, std::move(artistId));
    }
    RequestId albumTracks(std::string albumId, Done<RemoteTrack> done) override {
        return add(std::move(done), Kind::AlbumTracks, std::move(albumId));
    }
    // Answered only by replyBook() (or fail()): the fake knows no book by itself.
    RequestId book(std::string albumId, std::function<void(Error, RemoteBook)> done) override {
        return add(BookDone{std::move(done)}, Kind::Book, std::move(albumId));
    }
    Catalogue catalogue() const override { return catalogueKind; }
    bool syncsProgress() const override { return progressSync; }
    RequestId progress(std::string albumId, std::function<void(Error, std::optional<RemoteProgress>)> done) override {
        return add(std::move(done), Kind::Progress, std::move(albumId));
    }
    void saveProgress(std::string albumId, RemoteProgress progress, std::function<void(Error)> done = {}) override {
        if (!isConfigured) {
            if (done) ready_.emplace_back(0, [done = std::move(done)] { done(Error::NotConfigured); });
            return;
        }
        progressSaves.push_back({std::move(albumId), progress});
        if (!done) return;
        if (holdProgressSaves) heldProgressSaves_.push_back(std::move(done));
        else ready_.emplace_back(0, [done = std::move(done), error = progressSaveError] { done(error); });
    }
    bool answerProgressSave(Error error = Error::None) {
        if (heldProgressSaves_.empty()) return false;
        ready_.emplace_back(0, [done = std::move(heldProgressSaves_.front()), error] { done(error); });
        heldProgressSaves_.erase(heldProgressSaves_.begin());
        return true;
    }
    void dropHeldProgressSaves() { heldProgressSaves_.clear(); }
    bool replyProgress(RequestId id, std::optional<RemoteProgress> progress) {
        auto it = pending_.find(id);
        if (it == pending_.end()) return false;
        auto* done = std::get_if<ProgressDone>(&it->second);
        if (!done) throw std::logic_error("FakeMusicServer::replyProgress: not a progress request");
        ready_.emplace_back(id, [cb = std::move(*done), progress] { cb(Error::None, progress); });
        pending_.erase(it);
        return true;
    }
    RequestId inProgress(Done<RemoteAlbum> done) override { return add(std::move(done), Kind::InProgress); }
    RequestId playlists(Done<RemotePlaylist> done) override { return add(std::move(done), Kind::Playlists); }
    RequestId playlistTracks(std::string playlistId, Done<RemoteTrack> done) override {
        return add(std::move(done), Kind::PlaylistTracks, std::move(playlistId));
    }
    RequestId createPlaylist(std::string name, std::vector<std::string> trackIds, std::function<void(Error, std::string)> done) override {
        RequestId id = add(CreateDone{std::move(done)}, Kind::CreatePlaylist, std::move(name));
        requests.back().trackIds = std::move(trackIds);
        if (requests.back().trackIds.size() > kMaxPlaylistTracks && isConfigured) fail(id, Error::Server); // as Subsonic
        return id;
    }
    void cancel(RequestId id) override {
        cancelled.push_back(id); // poll() skips it, even if answered or polled right now
        pending_.erase(id);
    }
    // "fake://<streamHost>/<id>" ("fake://stream/<id>") for the original file; a transcode adds "?maxBitRate=<kbps>",
    // and its offset URL "&timeOffset=" after that. With saltUrls each call gives
    // another URL ("#<n>" at the end), as a real server's fresh salt does.
    Stream stream(const std::string& trackId, int maxBitRate) override {
        std::string url = "fake://" + streamHost + "/" + trackId;
        if (maxBitRate > 0) url += "?maxBitRate=" + std::to_string(maxBitRate);
        if (saltUrls) url += "#" + std::to_string(++streams_);
        if (maxBitRate <= 0) return {url, std::nullopt};
        return {url, Transcoded{url + "&timeOffset=", 0}};
    }
    void scrobble(const std::string& trackId, bool submission, std::optional<std::int64_t> timeMs,
                  std::function<void(Error)> done) override {
        if (!isConfigured) return;
        scrobbles.push_back({trackId, submission, timeMs});
        if (onScrobble) onScrobble(scrobbles.back()); // e.g. to look at the disk at the moment of the call
        if (!done) return;
        if (holdScrobbles) heldScrobbles_.push_back(std::move(done));
        else ready_.emplace_back(0, [done = std::move(done), error = scrobbleError] { done(error); });
    }
    // Answers the oldest held scrobble on the next poll(); false if none waits.
    bool answerScrobble(Error error = Error::None) {
        if (heldScrobbles_.empty()) return false;
        ready_.emplace_back(0, [done = std::move(heldScrobbles_.front()), error] { done(error); });
        heldScrobbles_.erase(heldScrobbles_.begin());
        return true;
    }
    void dropHeldScrobbles() { heldScrobbles_.clear(); } // the answers never come (the App that asked is gone)
    std::size_t heldScrobbleCount() const { return heldScrobbles_.size(); }
    void poll() override {
        // Callbacks may make new requests or cancel others, so work on a copy.
        auto batch = std::move(ready_);
        ready_.clear();
        for (auto& [id, run] : batch)
            if (std::find(cancelled.begin(), cancelled.end(), id) == cancelled.end()) run();
    }

    // Answer a list request; the callback gets it on the next poll(). The item
    // type must match the request (albums for Albums, ...). Returns false, and
    // does nothing, when the request is not pending (cancelled, already
    // answered, or never made): that is how a late answer is simulated.
    bool reply(RequestId id, std::vector<RemoteAlbum> items, bool more = false) {
        return deliver(id, std::move(items), more);
    }
    bool reply(RequestId id, std::vector<RemoteArtist> items, bool more = false) {
        return deliver(id, std::move(items), more);
    }
    bool reply(RequestId id, std::vector<RemoteTrack> items, bool more = false) {
        return deliver(id, std::move(items), more);
    }
    bool reply(RequestId id, std::vector<RemoteFolder> items, bool more = false) {
        return deliver(id, std::move(items), more);
    }
    bool reply(RequestId id, std::vector<RemotePlaylist> items, bool more = false) {
        return deliver(id, std::move(items), more);
    }
    bool replyPing(RequestId id, ServerInfo info) {
        auto it = pending_.find(id);
        if (it == pending_.end()) return false;
        auto* done = std::get_if<PingDone>(&it->second);
        if (!done) throw std::logic_error("FakeMusicServer::replyPing: not a ping");
        ready_.emplace_back(id, [cb = std::move(*done), info = std::move(info)] { cb(Error::None, info); });
        pending_.erase(it);
        return true;
    }
    // Answer a createPlaylist: it worked, and the server gave the new playlist this id (empty: it did not say).
    bool replyCreated(RequestId id, std::string playlistId = "new-playlist") {
        auto it = pending_.find(id);
        if (it == pending_.end()) return false;
        auto* done = std::get_if<CreateDone>(&it->second);
        if (!done) throw std::logic_error("FakeMusicServer::replyCreated: not a createPlaylist");
        ready_.emplace_back(id, [cb = std::move(done->call), playlistId = std::move(playlistId)] { cb(Error::None, playlistId); });
        pending_.erase(it);
        return true;
    }
    bool replyBook(RequestId id, RemoteBook book) {
        auto it = pending_.find(id);
        if (it == pending_.end()) return false;
        auto* done = std::get_if<BookDone>(&it->second);
        if (!done) throw std::logic_error("FakeMusicServer::replyBook: not a book request");
        ready_.emplace_back(id, [cb = std::move(done->call), book = std::move(book)]() mutable { cb(Error::None, std::move(book)); });
        pending_.erase(it);
        return true;
    }
    bool replyVersion(RequestId id, std::string version) {
        auto it = pending_.find(id);
        if (it == pending_.end()) return false;
        auto* done = std::get_if<VersionDone>(&it->second);
        if (!done) throw std::logic_error("FakeMusicServer::replyVersion: not a catalogueVersion request");
        ready_.emplace_back(id, [cb = std::move(*done), version = std::move(version)] { cb(Error::None, version); });
        pending_.erase(it);
        return true;
    }
    // Answer any request with an error (empty list, more = false).
    bool fail(RequestId id, Error error) {
        if (error == Error::None) throw std::logic_error("FakeMusicServer::fail: Error::None is not a failure");
        auto it = pending_.find(id);
        if (it == pending_.end()) return false;
        std::visit([&](auto& cb) { ready_.emplace_back(id, failure(std::move(cb), error)); }, it->second);
        pending_.erase(it);
        return true;
    }

    const Request& lastRequest() const {
        if (requests.empty()) throw std::logic_error("FakeMusicServer: no request made");
        return requests.back();
    }
    std::size_t pendingCount() const { return pending_.size(); }

    std::vector<Request> requests;    // every request, in order
    std::vector<RequestId> cancelled; // every cancel() call, in order
    struct Scrobble {
        Scrobble(std::string id, bool sub, std::optional<std::int64_t> time = std::nullopt)
            : trackId(std::move(id)), submission(sub), timeMs(time) {}
        std::string trackId;
        bool submission = false;
        std::optional<std::int64_t> timeMs; // when it was heard, if the call said; not part of ==, tests check it apart
        bool operator==(const Scrobble& o) const { return trackId == o.trackId && submission == o.submission; }
        friend void PrintTo(const Scrobble& s, std::ostream* out) {
            *out << s.trackId << (s.submission ? " submission" : " now playing");
        }
    };
    std::vector<Scrobble> scrobbles; // every scrobble() call, in order
    struct ProgressSave { std::string albumId; RemoteProgress progress; };
    std::vector<ProgressSave> progressSaves;
    bool progressSync = false;
    Error progressSaveError = Error::None;
    bool holdProgressSaves = false;
    bool isConfigured = true;
    Catalogue catalogueKind = Catalogue::Music; // what catalogue() says
    std::string streamHost = "stream"; // "fake://<streamHost>/<id>": what tells two fakes' streams apart
    bool saltUrls = false;
    Error scrobbleError = Error::None; // what the answer to a scrobble() says
    bool holdScrobbles = false;        // answers wait for answerScrobble()
    std::function<void(const Scrobble&)> onScrobble; // called by scrobble(), after it is recorded

private:
    std::vector<std::function<void(Error)>> heldScrobbles_;
    std::vector<std::function<void(Error)>> heldProgressSaves_;
    using ProgressDone = std::function<void(Error, std::optional<RemoteProgress>)>;
    using PingDone = std::function<void(Error, ServerInfo)>;
    using VersionDone = std::function<void(Error, std::string)>;
    struct CreateDone { // a type of its own: VersionDone has the same signature
        std::function<void(Error, std::string)> call;
    };
    struct BookDone { // a type of its own, like CreateDone
        std::function<void(Error, RemoteBook)> call;
    };
    using Callback =
        std::variant<PingDone, VersionDone, CreateDone, BookDone, ProgressDone, Done<RemoteAlbum>, Done<RemoteFolder>, Done<RemoteArtist>, Done<RemoteTrack>,
                 Done<RemotePlaylist>>;

    template <class F> RequestId add(F done, Kind kind, std::string arg = {}, int offset = 0, int size = 0, std::string folder = {}) {
        RequestId id = ++lastId_;
        requests.push_back({id, kind, std::move(arg), offset, size, std::move(folder), {}});
        if (isConfigured)
            pending_.emplace(id, Callback(std::in_place_type<F>, std::move(done)));
        else
            ready_.emplace_back(id, failure(std::move(done), Error::NotConfigured));
        return id;
    }
    template <class T> bool deliver(RequestId id, std::vector<T> items, bool more) {
        auto it = pending_.find(id);
        if (it == pending_.end()) return false;
        auto* done = std::get_if<Done<T>>(&it->second);
        if (!done) throw std::logic_error("FakeMusicServer::reply: wrong item type for request");
        ready_.emplace_back(id, [cb = std::move(*done), items = std::move(items), more]() mutable {
            cb(Error::None, std::move(items), more);
        });
        pending_.erase(it);
        return true;
    }
    static std::function<void()> failure(PingDone cb, Error error) {
        return [cb = std::move(cb), error] { cb(error, {}); };
    }
    static std::function<void()> failure(VersionDone cb, Error error) {
        return [cb = std::move(cb), error] { cb(error, {}); };
    }
    static std::function<void()> failure(CreateDone cb, Error error) {
        return [cb = std::move(cb.call), error] { cb(error, {}); };
    }
    static std::function<void()> failure(ProgressDone cb, Error error) {
        return [cb = std::move(cb), error] { cb(error, std::nullopt); };
    }
    static std::function<void()> failure(BookDone cb, Error error) {
        return [cb = std::move(cb.call), error] { cb(error, {}); };
    }
    template <class T> static std::function<void()> failure(Done<T> cb, Error error) {
        return [cb = std::move(cb), error] { cb(error, {}, false); };
    }

    RequestId lastId_ = 0;
    int streams_ = 0;
    std::map<RequestId, Callback> pending_;                          // asked, not answered yet
    std::vector<std::pair<RequestId, std::function<void()>>> ready_; // answered, waiting for poll()
};

// A Downloader that finishes only when the test says so, like FakeMusicServer.
// download() records the request and returns its id; nothing is written until the
// test calls finish(), which can also create the file.
//
//   auto id = downloader.lastRequest().id;
//   downloader.progress(id, 1000, 4000);       // optional
//   downloader.finish(id, Result::Ok);         // writes dest (Ok only), see below
//   app.tick();                                // App calls poll(), callbacks run
//
// `cancelled` lists every cancel() call. A cancelled id gets no callback even if
// the test still finishes it (finish() then returns false). With `isConfigured`
// false a download finishes at once with NotConfigured (still delivered by poll()).
class FakeDownloader : public Downloader {
public:
    struct Request {
        RequestId id = 0;
        std::string trackId, dest;
        long long expectedSize = 0;
        int bitrate = 0;
    };

    bool configured() const override { return isConfigured; }
    RequestId download(const std::string& trackId, const std::string& dest, long long expectedSize, Progress progress,
                       Done done, int bitrate = 0) override {
        RequestId id = ++lastId_;
        requests.push_back({id, trackId, dest, expectedSize, bitrate});
        if (isConfigured) pending_.emplace(id, Callbacks{std::move(progress), std::move(done)});
        else ready_.emplace_back(id, [done = std::move(done)] { if (done) done(Result::NotConfigured); });
        return id;
    }
    void cancel(RequestId id) override {
        cancelled.push_back(id);
        pending_.erase(id);
        std::erase_if(ready_, [id](const auto& r) { return r.first == id; });
    }
    void poll() override {
        auto batch = std::move(ready_);
        ready_.clear();
        for (auto& [id, run] : batch)
            if (std::find(cancelled.begin(), cancelled.end(), id) == cancelled.end()) run();
    }

    // Report progress; the callback runs on the next poll(). False if not pending.
    bool progress(RequestId id, long long bytes, long long total) {
        auto it = pending_.find(id);
        if (it == pending_.end()) return false;
        if (it->second.progress)
            ready_.emplace_back(id, [cb = it->second.progress, bytes, total] { cb(bytes, total); });
        return true;
    }
    // Finish a download; the callback runs on the next poll(). With Ok and
    // `writeFile`, `dest` is created with `contents` first (and its folders), as a
    // real download leaves the finished file. Other results leave a `.part` of `partial`
    // bytes when `partial` is not empty. False if the id is not pending (cancelled,
    // finished, or never made): that is how a late answer is simulated.
    bool finish(RequestId id, Result result, bool writeFile = true, const std::string& contents = "audio",
                const std::string& partial = "") {
        auto it = pending_.find(id);
        if (it == pending_.end()) return false;
        const std::string dest = destOf(id);
        if (result == Result::Ok && writeFile) write(dest, contents);
        else if (result != Result::Ok && !partial.empty()) write(dest + ".part", partial);
        ready_.emplace_back(id, [cb = std::move(it->second.done), result] { if (cb) cb(result); });
        pending_.erase(it);
        return true;
    }

    const Request& lastRequest() const {
        if (requests.empty()) throw std::logic_error("FakeDownloader: no download requested");
        return requests.back();
    }
    std::size_t pendingCount() const { return pending_.size(); }

    std::vector<Request> requests;    // every download(), in order
    std::vector<RequestId> cancelled; // every cancel() call, in order
    bool isConfigured = true;

private:
    struct Callbacks {
        Progress progress;
        Done done;
    };
    std::string destOf(RequestId id) const {
        for (const auto& r : requests)
            if (r.id == id) return r.dest;
        return {};
    }
    static void write(const std::string& path, const std::string& contents) {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        std::ofstream(path, std::ios::binary) << contents;
    }

    RequestId lastId_ = 0;
    std::map<RequestId, Callbacks> pending_;
    std::vector<std::pair<RequestId, std::function<void()>>> ready_;
};

} // namespace oap::test
