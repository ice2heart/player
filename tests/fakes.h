#pragma once

// Test doubles for the interfaces in core/services.h. They record what App asked
// for and let the test set what App will observe (time, playback position, ...).

#include "core/services.h"
#include "core/settings.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oap::test {

class FakeClock : public Clock {
public:
    TimePoint now() const override { return now_; }
    std::time_t wallTime() const override { return 1'700'000'000; }
    void advance(std::chrono::milliseconds ms) { now_ += ms; }

private:
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
// and then it opens like any other stream. As on the real Player, a preload of
// another path replaces it and cancelPreload() drops it; an open() of another
// path and close() leave it. `preloads` records every call, local paths too
// (which the player ignores).
//
// An open with `transcoded` records it; like the real Player, duration() is then
// the one given there, not `length`.
class FakePlayer : public MediaPlayer {
public:
    bool open(const std::string& path, double startAt, std::optional<Transcoded> how) override {
        if (beforeOpen) beforeOpen(path);
        opened.push_back(path);
        transcoded = std::move(how);
        bool taken = preloaded == path;
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
    void seek(double seconds) override { pos = seconds; }
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
// scrobble() is recorded in `scrobbles`, in order (nothing comes back from it).
class FakeMusicServer : public MusicServer {
public:
    enum class Kind { Ping, Albums, Artists, ArtistAlbums, AlbumTracks, Playlists, PlaylistTracks };
    struct Request {
        RequestId id = 0;
        Kind kind = Kind::Ping;
        std::string arg; // listType, artist, album or playlist id; empty otherwise
        int offset = 0;  // Albums only
        int size = 0;    // Albums only
    };

    bool configured() const override { return isConfigured; }
    RequestId ping(std::function<void(Error, ServerInfo)> done) override { return add(std::move(done), Kind::Ping); }
    RequestId albums(std::string listType, int offset, int size, Done<RemoteAlbum> done) override {
        return add(std::move(done), Kind::Albums, std::move(listType), offset, size);
    }
    RequestId artists(Done<RemoteArtist> done) override { return add(std::move(done), Kind::Artists); }
    RequestId artistAlbums(std::string artistId, Done<RemoteAlbum> done) override {
        return add(std::move(done), Kind::ArtistAlbums, std::move(artistId));
    }
    RequestId albumTracks(std::string albumId, Done<RemoteTrack> done) override {
        return add(std::move(done), Kind::AlbumTracks, std::move(albumId));
    }
    RequestId playlists(Done<RemotePlaylist> done) override { return add(std::move(done), Kind::Playlists); }
    RequestId playlistTracks(std::string playlistId, Done<RemoteTrack> done) override {
        return add(std::move(done), Kind::PlaylistTracks, std::move(playlistId));
    }
    void cancel(RequestId id) override {
        cancelled.push_back(id); // poll() skips it, even if answered or polled right now
        pending_.erase(id);
    }
    // "fake://stream/<id>" for the original file; a transcode adds "?maxBitRate=<kbps>",
    // and its offset URL "&timeOffset=" after that. With saltUrls each call gives
    // another URL ("#<n>" at the end), as a real server's fresh salt does.
    Stream stream(const std::string& trackId, int maxBitRate) override {
        std::string url = "fake://stream/" + trackId;
        if (maxBitRate > 0) url += "?maxBitRate=" + std::to_string(maxBitRate);
        if (saltUrls) url += "#" + std::to_string(++streams_);
        if (maxBitRate <= 0) return {url, std::nullopt};
        return {url, Transcoded{url + "&timeOffset=", 0}};
    }
    void scrobble(const std::string& trackId, bool submission) override { scrobbles.push_back({trackId, submission}); }
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
        std::string trackId;
        bool submission = false;
        bool operator==(const Scrobble&) const = default;
        friend void PrintTo(const Scrobble& s, std::ostream* out) {
            *out << s.trackId << (s.submission ? " submission" : " now playing");
        }
    };
    std::vector<Scrobble> scrobbles; // every scrobble() call, in order
    bool isConfigured = true;
    bool saltUrls = false;

private:
    using PingDone = std::function<void(Error, ServerInfo)>;
    using Callback =
        std::variant<PingDone, Done<RemoteAlbum>, Done<RemoteArtist>, Done<RemoteTrack>, Done<RemotePlaylist>>;

    template <class F> RequestId add(F done, Kind kind, std::string arg = {}, int offset = 0, int size = 0) {
        RequestId id = ++lastId_;
        requests.push_back({id, kind, std::move(arg), offset, size});
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
    template <class T> static std::function<void()> failure(Done<T> cb, Error error) {
        return [cb = std::move(cb), error] { cb(error, {}, false); };
    }

    RequestId lastId_ = 0;
    int streams_ = 0;
    std::map<RequestId, Callback> pending_;                          // asked, not answered yet
    std::vector<std::pair<RequestId, std::function<void()>>> ready_; // answered, waiting for poll()
};

} // namespace oap::test
