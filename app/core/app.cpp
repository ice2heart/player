#include "app.h"

#include "album_merge.h"

#include "text.h"
#include "track_cache.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <set>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

namespace oap {

namespace {
constexpr const char* kVersion = "0.1";
constexpr size_t kSpeechLogSize = 100;

// Setting keys
constexpr const char* kSpeechRate = "speech.rate";
constexpr const char* kVolume = "volume";
constexpr const char* kVerbosity = "verbosity"; // 0 beginner, 1 normal, 2 expert
constexpr const char* kSkipBack = "skip.back";
constexpr const char* kSkipForward = "skip.forward";
constexpr const char* kRewindOnResume = "rewind_on_resume";
constexpr const char* kLongPress = "buttons.long_press_ms";
constexpr const char* kMenuWrap = "menu.wrap";
constexpr const char* kAutoSleep = "power.auto_sleep_min";
constexpr const char* kStreamingBitrate = "streaming.bitrate"; // kbit/s of a transcode; 0: the original file
constexpr const char* kCacheLimit = "cache.limit_gb"; // automatic cache: gigabytes of unpinned tracks, 0: off
constexpr const char* kLastBook = "last_book";
// The catalogue version of the server the index was last synced with (getArtists `lastModified`),
// and the server key it belongs to. In state.tsv, never in config.yaml.
constexpr const char* kCatalogueServer = "catalogue.server";
constexpr const char* kCatalogueVersion = "catalogue.version";
constexpr int kCataloguePage = 500;          // albums per request (the most getAlbumList2 gives)
constexpr size_t kCatalogueLimit = 200000;   // a server that never stops paging is an error
constexpr const char* kServerName = "server.name"; // the Streaming entry on Home
constexpr const char* kServerUrl = "server.url";
constexpr const char* kServerUser = "server.user";
// The remote list that played last, resumed by "Now playing" after a restart.
// The server is named by its url and user (never the password, nor a stream
// URL, which holds the login), so a place on another server is not resumed.
constexpr const char* kLastStream = "last_stream.";
constexpr const char* kLastStreamServer = "last_stream.server";
constexpr const char* kLastStreamUser = "last_stream.user";
constexpr const char* kLastStreamKind = "last_stream.kind"; // "album" or "playlist"
constexpr const char* kLastStreamId = "last_stream.id";
constexpr const char* kLastStreamTitle = "last_stream.title";
constexpr const char* kLastStreamTrack = "last_stream.track"; // index in the list
constexpr const char* kLastStreamTrackId = "last_stream.track_id";
constexpr const char* kLastStreamPos = "last_stream.pos"; // seconds into the track

// Lists from the server (docs/streaming.md). Albums come in pages of the most
// a Subsonic server sends at once; Random albums is one page, since each page
// is a new random draw and would repeat albums.
constexpr int kAlbumPageSize = 500;
constexpr int kRandomAlbums = 50;
// Player gives up a stream open after 10 s, but a wait it cannot interrupt (a
// DNS lookup) can take longer. App gives up a little after it, so "Loading"
// always ends.
constexpr std::chrono::seconds kStreamOpenGiveUp{15};
// A stream that plays and has run dry (MediaPlayer::buffering()). After a seek it
// is empty for a moment while the new part loads: only a longer gap gets the
// buffering earcon. A dropout that lasts says so once; Player retries meanwhile,
// and if it gives up, failed() says "Server not reachable". On a slow link the
// audio comes in bits: only after it has played this long without a gap is the
// dropout over, so the stutter gets one earcon and then the phrase.
constexpr std::chrono::milliseconds kDryBeforeEarcon{500};
constexpr std::chrono::seconds kDryBeforeWaitingForNetwork{3};
constexpr std::chrono::seconds kPlayingBeforeRecovered{2};
// Streaming quality: the original file (Range seeks, any format), or an MP3
// transcode for a slow link (docs/streaming.md, step 2.4).
constexpr int kOriginalQuality = 0;
const std::vector<std::pair<int, std::string>> kStreamingQualities = {{kOriginalQuality, "original file"},
                                                                      {320, "320 kilobits per second"},
                                                                      {192, "192 kilobits per second"},
                                                                      {128, "128 kilobits per second"}};
// Automatic cache: a streamed track is fetched a second time into the cache once it
// has been heard this long, so a track that is skipped costs no bandwidth.
constexpr std::chrono::seconds kAutoCacheAfter{10};
// Free space the automatic cache never uses: room for state.tsv, the config, the manifests (each
// written through a temporary copy) and the logs, on a small card that the cache must not fill.
// Those are a few kilobytes; 256 MiB also leaves a margin for a track whose size the server did
// not list and for the file system's own overhead.
constexpr std::uintmax_t kAutoCacheReserve = std::uintmax_t(256) << 20;
constexpr int kDefaultCacheGigabytes = 1;
// Read-ahead: while a remote track plays its last this long, the next one's stream
// opens in the background (MediaPlayer::preload()), so the next track starts
// without waiting for an open on a slow link.
constexpr std::chrono::seconds kReadAhead{10};
// Scrobbling: a remote track counts as played on the server ("Recently played",
// play counts) once it has been heard for this share of its length, or for
// kScrobbleAtMost if that comes first (docs/streaming.md, step 2.5).
constexpr double kScrobbleShare = 0.5;
constexpr std::chrono::minutes kScrobbleAtMost{4};
constexpr const char* kNoServerHelp =
    "To stream from your own music server, add a server section with its url, user and password to "
    "config.yaml, then restart the player.";
// Also what a build without network support says (OAP_BUILD_NET=OFF): core cannot tell the two apart.
constexpr const char* kUnsupportedServerHelp =
    "This player streams only from Subsonic servers, such as Navidrome, and only when it is built with network "
    "support. Check the type in the server section of config.yaml.";

enum Verbosity { Beginner = 0, Normal = 1, Expert = 2 };

// A place in seconds as state.tsv keeps it: "42.5".
std::string tenths(double seconds) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", seconds);
    return text;
}

// The same server with or without a slash at the end of its url.
std::string serverIdentity(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

std::vector<std::pair<int, std::string>> range(int from, int to, int step, const std::string& unit) {
    std::vector<std::pair<int, std::string>> out;
    for (int v = from; v <= to; v += step) out.push_back({v, std::to_string(v) + unit});
    return out;
}

std::vector<std::pair<int, std::string>> seconds(std::initializer_list<int> values) {
    std::vector<std::pair<int, std::string>> out;
    for (int v : values) out.push_back({v, v == 0 ? "off" : spokenDuration(v)});
    return out;
}

// "01 - The Storm" -> "The Storm": track numbers are noise when spoken.
std::string chapterTitle(const std::filesystem::path& file) {
    std::string name = file.stem().string();
    size_t i = 0;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i == 0) return name;
    while (i < name.size() && (name[i] == ' ' || name[i] == '-' || name[i] == '.' || name[i] == '_')) ++i;
    return i < name.size() ? name.substr(i) : name;
}

// The spoken name of part `i`. A remote part is never named after its file,
// which is the server's track id.
std::string partTitle(const Book& book, size_t i) {
    if (i < book.titles.size() && !book.titles[i].empty()) return book.titles[i];
    if (book.remote) return "Track " + std::to_string(i + 1);
    return chapterTitle(book.files[i]);
}

struct ButtonHelp {
    Button button;
    const char* name;
    const char* text;
};

const ButtonHelp kButtonHelp[] = {
    {Button::PlayPause, "Play pause", "Starts or pauses playback. Hold to hear where you are."},
    {Button::Back, "Left", "In menus, lowers a setting. In player, skip back. Hold, or press deeper, for previous chapter or track."},
    {Button::Forward, "Right", "In menus, raises a setting. In player, skip forward. Hold, or press deeper, for next chapter or track."},
    {Button::Up, "Up", "In menus, previous item. Hold for first item. In player, volume up."},
    {Button::Down, "Down", "In menus, next item. Hold for last item. In player, volume down."},
    {Button::Ok, "OK", "Opens the selected item. In player, opens the player menu. Hold to go back, like Home."},
    {Button::Home, "Home", "Goes back to the previous screen. Hold for the home menu."},
    {Button::Talk, "Talk", "Hold and speak a command. Coming later."},
    {Button::Power, "Power", "Press to sleep. Hold for the power menu."},
};

std::string keyHelp(Button button) {
    for (const ButtonHelp& h : kButtonHelp)
        if (h.button == button) return std::string(h.name) + ". " + h.text;
    return "";
}

struct SleepTime {
    const char* label;
    int minutes;
};

const SleepTime kSleepTimes[] = {{"Off", 0},          {"1 minute", 1},     {"15 minutes", 15},
                                 {"30 minutes", 30}, {"45 minutes", 45}, {"1 hour", 60}};

Screen menuScreen(std::string title, std::function<std::vector<MenuItem>()> build) {
    std::vector<MenuItem> items = build();
    return {Screen::Kind::Menu, std::move(title), std::move(items), std::move(build)};
}

Screen playerScreen() { return {Screen::Kind::Player, "Now playing", {}, {}}; }

// Room left on the disk of `folder`, which must exist.
std::optional<std::uintmax_t> spaceOnDisk(const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::space_info info = std::filesystem::space(folder, ec);
    if (ec) return std::nullopt;
    return info.available;
}

// "1 track", "3 tracks"
std::string countValue(size_t n, const std::string& unit) { return std::to_string(n) + " " + unit + (n == 1 ? "" : "s"); }

// What a failed request is called in a list, and what the user can do about it.
std::string errorText(MusicServer::Error error) {
    switch (error) {
    case MusicServer::Error::NotConfigured: return "No server set up";
    case MusicServer::Error::Unreachable: return "Server not reachable";
    case MusicServer::Error::BadLogin: return "Wrong user name or password";
    case MusicServer::Error::Server: return "Server error";
    case MusicServer::Error::None: break;
    }
    return "";
}

std::string errorHelp(MusicServer::Error error) {
    switch (error) {
    case MusicServer::Error::NotConfigured: return kNoServerHelp;
    case MusicServer::Error::Unreachable:
        return "Check that Wi-Fi is on and the server is running, then choose Try again.";
    case MusicServer::Error::BadLogin: return "Check the user and password in the server section of config.yaml.";
    case MusicServer::Error::Server: return "The server could not answer. Choose Try again, or try later.";
    case MusicServer::Error::None: break;
    }
    return "";
}

// "navidrome", "0.64.2" -> "Connected to Navidrome 0.64.2."
std::string connectedText(const MusicServer::ServerInfo& info) {
    std::string name = info.type.empty() ? "the server" : info.type;
    if (!info.type.empty()) name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    return "Connected to " + name + (info.version.empty() ? "" : " " + info.version) + ".";
}

MenuItem action(std::string label, std::function<void()> fn) {
    MenuItem item;
    item.label = std::move(label);
    item.action = std::move(fn);
    return item;
}

MenuItem submenu(std::string label, std::function<std::vector<MenuItem>()> fn) {
    MenuItem item;
    item.label = std::move(label);
    item.submenu = std::move(fn);
    return item;
}
} // namespace

App::App(Speech& speech, MediaPlayer& player, SoundEffects& sounds, VolumeControl& volume, MusicServer& server,
         Downloader& downloader, LibraryStore& libraryStore, LibraryScanner& scanner, Settings& config, Settings& state,
         Clock& clock, std::filesystem::path libraryRoot, std::filesystem::path musicRoot, std::filesystem::path cacheRoot,
         TextFiles* files)
    : tts_(speech), player_(player), earcons_(sounds), volume_(volume), server_(server), downloader_(downloader),
      index_(libraryStore), scanner_(scanner), config_(config), state_(state),
      savedState_(state.values()), clock_(clock), libraryRoot_(std::move(libraryRoot)), musicRoot_(std::move(musicRoot)), cacheRoot_(std::move(cacheRoot)),
      freeSpace_(spaceOnDisk), lastActivity_(clock.now()) {
    textFiles_ = files;
}

// ---------------------------------------------------------------- lifecycle

void App::start() {
    tts_.setRate(config_.getInt(kSpeechRate, 220));
    volume_.setVolume(config_.getInt(kVolume, 70) / 100.0f);
    gestures_.setLongPress(std::chrono::milliseconds(config_.getInt(kLongPress, 600)));
    lastActivity_ = clock_.now();

    earcons_.play(Earcon::Boot);
    say("Ready.");
    startScan(); // the menus use the saved index until it is done
    startSync(); // silent: the saved server items stay if it fails

    // A stream played last: "Now playing" on Home resumes it. Nothing is asked
    // of the server now, and nothing plays until the user chooses it.
    if (savedStream()) {
        goHome();
        return;
    }
    std::string last = state_.getString(kLastBook);
    if (auto b = resumableBook(last); b && openBook(*b, false)) {
        closeScreens(0);
        pushScreen(menuScreen("Home", [this] { return homeMenu(); }));
        pushScreen(playerScreen());
        say(b->title + ", paused. Press play to continue.");
        return;
    }
    goHome();
}

// The book `last_book` names, if it is still there: from the index, and when the index does not
// know it yet (the first start after an update, before the first scan is done) from the disk.
std::optional<Book> App::resumableBook(const std::string& path) const {
    if (path.empty()) return std::nullopt;
    if (const IndexItem* item = index_.find(kLocalBookPrefix + path)) {
        Book book = bookOf(*item);
        std::error_code ec;
        for (const auto& file : book.files)
            if (!std::filesystem::exists(file, ec)) return std::nullopt; // gone since the last scan
        return book;
    }
    for (const Book& b : scanLibrary(libraryRoot_, cacheRoot_))
        if (b.path.string() == path) return b;
    return std::nullopt;
}

void App::startScan() {
    ScanRequest request;
    request.libraryRoot = libraryRoot_;
    request.musicRoot = musicRoot_;
    request.skip = cacheRoot_;
    for (const auto& [id, item] : index_.items())
        if (item.source == IndexItem::Source::Local) request.previous.push_back(item);
    scanRequest_ = scanner_.scan(std::move(request), [this](std::vector<IndexItem> items) {
        scanRequest_ = 0;
        onScanned(std::move(items));
    });
}

// The scan is the local files as they are now: the index is made to match, and written only if that changed it.
void App::onScanned(std::vector<IndexItem> items) {
    index_.replaceSource(IndexItem::Source::Local, items, clock_.wallTime());
    linkToServer();
    index_.save();
    finishRefresh();
}

// ---------------------------------------------------------------- server catalogue

std::string App::catalogueServerKey() const {
    return TrackCache::serverKey(config_.getString(kServerUrl), config_.getString(kServerUser)); // empty: no server set up
}

namespace {
std::string keyOfRef(const std::string& ref) { return ref.substr(0, ref.find(':')); }
} // namespace

// What was seen on another server (or the same one under another user), or on any server once
// none is set up, is not this server's: its server items and its references go. Items on this
// device stay.
void App::dropOtherServers(const std::string& key) {
    std::set<std::string> others;
    for (const auto& [id, item] : index_.items()) {
        for (const auto& ref : item.seenOn) others.insert(keyOfRef(ref));
        for (const auto& track : item.tracks)
            for (const auto& ref : track.seenOn) others.insert(keyOfRef(ref));
    }
    others.erase(key);
    for (const auto& other : others) index_.replaceServer(other, {}, clock_.wallTime());
    index_.save();
}

namespace {
std::string downloadedName(const CachedList& list); // below, with the downloaded lists
}

// The pinned albums of the cache are the Cached items of the index. A download carries the
// reference of the server it came from, taken from its manifest: the album's own and one per
// track. The reference is kept only while the server still lists the album (or while no
// catalogue of this server is known yet), because an album that left the server has no
// reference. Playlists are not albums and are not in the index.
void App::syncDownloads() {
    std::vector<IndexItem> items;
    const TrackCache& store = trackCache();
    if (store.enabled()) {
        std::string key = catalogueServerKey();
        bool catalogue = false;
        for (const auto& [id, item] : index_.items())
            if (item.source == IndexItem::Source::Server && LibraryIndex::refOnServer(item.seenOn, key)) catalogue = true;
        for (const CachedList& list : store.lists(true)) {
            if (list.source.kind != RemoteSource::Kind::Album) continue;
            IndexItem item;
            std::string ref = LibraryIndex::seenRef(key, SeenKind::Album, list.source.id);
            item.id = "cached:" + ref;
            item.source = IndexItem::Source::Cached;
            item.kind = IndexItem::Kind::Music;
            item.title = downloadedName(list);
            item.album = list.name; // not the placeholder: it must not feed the name rule
            item.artist = list.artist;
            for (const CachedTrack& t : list.tracks) {
                auto file = store.completeFile(list.source, t);
                if (!file) continue;
                IndexTrack track;
                track.location = file->string();
                track.title = t.title;
                track.duration = t.duration;
                track.seenOn.push_back(LibraryIndex::seenRef(key, SeenKind::Track, t.id));
                item.tracks.push_back(std::move(track));
            }
            if (item.tracks.empty()) continue;
            item.trackCount = int(item.tracks.size());
            const IndexItem* onServer = index_.find(ref);
            if (!catalogue || (onServer && onServer->source == IndexItem::Source::Server)) item.seenOn.push_back(ref);
            items.push_back(std::move(item));
        }
    }
    index_.replaceSource(IndexItem::Source::Cached, items, clock_.wallTime());
}

// Local and downloaded items with no reference to the server that are a server album by the
// name rule get its reference (merged entries keep it, so a later sync matches by it).
void App::linkToServer() {
    std::string key = catalogueServerKey();
    if (key.empty()) return;
    for (const AlbumLink& link : linkByName(key, index_.items())) index_.addSeenOn(link.itemId, link.reference);
}

void App::startSync() {
    std::string key = catalogueServerKey();
    dropOtherServers(key); // with no server set up that is every server
    if (key.empty() || !server_.configured() || catalogueRequest_) return; // nothing to sync, or a sync is running
    catalogue_ = {};
    catalogue_.key = key;
    catalogueRequest_ = server_.catalogueVersion([this](MusicServer::Error error, std::string version) {
        catalogueRequest_ = 0;
        onCatalogueVersion(error, version);
    });
}

void App::onCatalogueVersion(MusicServer::Error error, const std::string& version) {
    if (error != MusicServer::Error::None) return finishSync(error);
    bool haveItems = false;
    for (const auto& [id, item] : index_.items())
        if (item.source == IndexItem::Source::Server && LibraryIndex::refOnServer(item.seenOn, catalogue_.key))
            haveItems = true;
    // A server that gives no version cannot be told unchanged. Without server items in the
    // index (it was lost) the version does not matter either.
    if (!version.empty() && haveItems && state_.getString(kCatalogueServer) == catalogue_.key &&
        state_.getString(kCatalogueVersion) == version)
        return finishSync(MusicServer::Error::None);
    catalogue_.version = version;
    fetchCatalogue(0);
}

void App::fetchCatalogue(int offset) {
    catalogueRequest_ = server_.albums(
        "alphabeticalByName", offset, kCataloguePage,
        [this, offset](MusicServer::Error error, std::vector<RemoteAlbum> albums, bool more) {
            catalogueRequest_ = 0;
            if (error != MusicServer::Error::None) return finishSync(error);
            for (const RemoteAlbum& a : albums) {
                IndexItem item;
                item.id = LibraryIndex::seenRef(catalogue_.key, SeenKind::Album, a.id);
                item.source = IndexItem::Source::Server;
                item.kind = IndexItem::Kind::Music;
                item.title = a.name;
                item.artist = a.artist;
                item.album = a.name;
                item.genre = a.genre;
                item.year = a.year;
                item.musicBrainzId = a.musicBrainzId;
                item.trackCount = a.songCount;
                item.created = parseIsoTime(a.created);
                item.seenOn.push_back(item.id);
                catalogue_.items.push_back(std::move(item));
            }
            // A page that is empty ends it however `more` says, and so does a server that never ends.
            if (more && !albums.empty() && catalogue_.items.size() < kCatalogueLimit)
                return fetchCatalogue(offset + int(albums.size()));
            if (more && !albums.empty()) return finishSync(MusicServer::Error::Server);
            applyCatalogue();
            finishSync(MusicServer::Error::None);
        });
}

// Every page came: the server items of the index become the albums fetched.
void App::applyCatalogue() {
    index_.replaceServer(catalogue_.key, catalogue_.items, clock_.wallTime());
    catalogue_.items.clear();
    syncDownloads(); // a download of an album the server dropped loses its reference
    linkToServer();
    if (index_.save() && !catalogue_.version.empty()) { // else the next start checks again
        state_.setString(kCatalogueServer, catalogue_.key);
        state_.setString(kCatalogueVersion, catalogue_.version);
    }
}

// The sync is over: with an error the index is as it was.
void App::finishSync(MusicServer::Error error) {
    catalogue_.items.clear();
    refresh_.error = error;
    finishRefresh();
}

// Music -> Refresh library: rescans the files and, with a server, checks its catalogue. A scan or
// sync that is already running (the start-up one) is joined, not started twice.
void App::refreshLibrary() {
    if (refresh_.active) {
        say("Still updating.");
        return;
    }
    refresh_ = {};
    refresh_.active = true;
    refresh_.screen = screens_.empty() ? 0 : screens_.back().id;
    say("Updating the library.");
    if (!scanRequest_) startScan();
    if (!catalogueRequest_) startSync();
}

// Called when a scan or a sync ends: when none is left, a Refresh that waits speaks its result,
// but only on the screen it started on and while awake (a late result is never spoken).
void App::finishRefresh() {
    if (!refresh_.active || scanRequest_ || catalogueRequest_) return;
    Refresh done = std::exchange(refresh_, {});
    if (sleeping_ || screens_.empty() || screens_.back().id != done.screen) return;
    if (done.error != MusicServer::Error::None) {
        say(errorText(done.error) + ".");
        return;
    }
    syncDownloads(); // a merged album counts once
    index_.save();
    say("Library updated, " + countValue(mergeAlbums(index_.items()).size(), "album") + ".");
}

void App::shutdown() {
    saveOnExit();
    cancelResume();
    if (download_) downloader_.cancel(download_->request); // the .part stays: OK on Download goes on from it
    download_.reset();
    if (autoRequest_) downloader_.cancel(std::exchange(autoRequest_, 0));
    autoJob_.reset();
    autoQueue_.clear();
    if (scanRequest_) scanner_.cancel(std::exchange(scanRequest_, 0));
    if (catalogueRequest_) server_.cancel(std::exchange(catalogueRequest_, 0));
    refresh_ = {};
    pendingOpen_.reset();
    player_.cancelPreload();
    player_.close();
}

void App::keyDown(Button button) {
    held_[static_cast<size_t>(button)] = true;
    if (hold_ && button != Button::Power) {
        tts_.stop();
        earcons_.play(Earcon::Error);
        say("Locked.");
        return;
    }
    if (auto ev = gestures_.press(button, clock_.now())) onGesture(*ev);
}

void App::keyUp(Button button) {
    held_[static_cast<size_t>(button)] = false;
    auto ev = gestures_.release(button, clock_.now());
    if (std::exchange(locked_[static_cast<size_t>(button)], false)) return; // held down when Hold was switched on
    if (ev) onGesture(*ev);
}

void App::onGesture(ButtonEvent ev) {
    if (!locked_[static_cast<size_t>(ev.button)]) onButton(ev);
}

void App::setHold(bool on) {
    if (hold_ == on) return;
    hold_ = on;
    // A key already down when Hold is switched on does nothing more: no long
    // press while it is held, no short press when it is let go. Power still works.
    if (on)
        for (size_t i = 0; i < kButtonCount; ++i)
            if (held_[i] && static_cast<Button>(i) != Button::Power) locked_[i] = true;
    tts_.stop();
    earcons_.play(on ? Earcon::Close : Earcon::Open);
    say(on ? "Buttons locked." : "Buttons unlocked.");
}

App::Mode App::mode() const {
    if (sleeping_) return Mode::Sleeping;
    return !screens_.empty() && screens_.back().kind == Screen::Kind::Player ? Mode::Player : Mode::Menu;
}

void App::tick() {
    auto now = clock_.now();
    for (const ButtonEvent& ev : gestures_.poll(now)) onGesture(ev);
    server_.poll(); // answers from the server fill their lists
    downloader_.poll(); // a finished track starts the next one
    scanner_.poll();    // a finished scan updates the index
    if (pendingOpen_ && !player_.opening()) finishPendingOpen();
    else if (pendingOpen_ && now - pendingOpen_->since > kStreamOpenGiveUp) giveUpPendingOpen();

    // The end of a file is always the end of a chapter, even when the next
    // chapter has the same number (a one-file book or a loose track starts again).
    if (book_ && player_.failed()) onStreamFailed();
    bool fileFinished = book_ && player_.finished();
    if (fileFinished) onFileFinished();
    checkBuffering(now);
    checkScrobble(now);
    checkAutoCache();
    pumpAutoCache();

    if (sleepDeadline_ && now >= *sleepDeadline_) {
        sleepDeadline_.reset();
        sleep("Sleep timer ended.");
    }
    // While a track opens, the current one is the track it would replace if it fails.
    if (sleepAtChapterEnd_ >= 0 && book_ &&
        (fileFinished || (!pendingOpen_ && currentChapter() != sleepAtChapterEnd_))) {
        sleepAtChapterEnd_ = -1;
        sleep("End of " + unit() + ". Sleep timer ended.");
    }

    int autoSleep = config_.getInt(kAutoSleep, 5);
    if (!sleeping_ && autoSleep > 0 && !player_.playing() && !pendingOpen_ && !tts_.speaking() &&
        now - lastActivity_ > std::chrono::minutes(autoSleep))
        sleep("Going to sleep.");
    checkReadAhead(); // last: it sees what this tick opened, and whether playback stopped
}

// ---------------------------------------------------------------- buttons

void App::onButton(ButtonEvent ev) {
    usage_[static_cast<size_t>(ev.button)][static_cast<size_t>(ev.gesture)]++;
    lastActivity_ = clock_.now();

    // A value changed with Left / Right is written once the user moves on.
    if (ev.button != Button::Back && ev.button != Button::Forward) saveChangedSettings();

    if (sleeping_) {
        wake();
        return;
    }
    // Left / Right on an item without a value do nothing at all, not even stop speech.
    bool idle = !keyDescriber_ && mode() == Mode::Menu && !screens_.empty() &&
                (ev.button == Button::Back || ev.button == Button::Forward) && !canAdjust();
    if (idle) return;
    tts_.stop(); // any other key interrupts speech

    if (keyDescriber_) {
        describeKey(ev);
        return;
    }

    switch (ev.button) {
    case Button::Power:
        if (ev.gesture == Gesture::Short) sleep("Going to sleep.");
        else openPowerMenu();
        return;
    case Button::Talk:
        earcons_.play(Earcon::Error);
        say("Voice input is not available yet.");
        return;
    case Button::PlayPause:
        if (ev.gesture == Gesture::Short) togglePlay();
        else whereAmI();
        return;
    default:
        break;
    }

    if (mode() == Mode::Player) handlePlayer(ev);
    else handleMenu(ev);
}

// OK goes in, Home goes out, Left / Right change a value (docs/navigation.md).
void App::handleMenu(ButtonEvent ev) {
    if (screens_.empty()) {
        goHome();
        return;
    }
    Screen& screen = screens_.back();
    bool longPress = ev.gesture == Gesture::Long;

    switch (ev.button) {
    case Button::Up:
    case Button::Down: {
        if (screen.items.empty()) {
            earcons_.play(Earcon::Edge);
            say(screen.title + ", empty.");
            return;
        }
        long count = long(screen.items.size());
        if (longPress) {
            screen.index = ev.button == Button::Up ? 0 : size_t(count - 1);
            earcons_.play(Earcon::Move);
        } else {
            long next = long(screen.index) + (ev.button == Button::Up ? -1 : 1);
            if (next < 0 || next >= count) {
                if (!config_.getInt(kMenuWrap, 1)) {
                    earcons_.play(Earcon::Edge);
                    say(itemSpeech(screen));
                    return;
                }
                next = (next + count) % count;
                earcons_.play(Earcon::Wrap);
            } else {
                earcons_.play(Earcon::Move);
            }
            screen.index = size_t(next);
        }
        say(itemSpeech(screen));
        return;
    }
    case Button::Back:
    case Button::Forward: // short, long and deep all step the value once
        adjust(ev.button == Button::Back ? -1 : 1);
        return;
    case Button::Ok:
        if (longPress) back();
        else activate();
        return;
    case Button::Home:
        if (longPress) goHome();
        else back();
        return;
    default:
        return;
    }
}

void App::handlePlayer(ButtonEvent ev) {
    switch (ev.button) {
    case Button::Back:
    case Button::Forward: {
        int dir = ev.button == Button::Back ? -1 : 1;
        if (ev.gesture == Gesture::Short) skip(dir);
        else stepChapter(dir); // long press, or the deep stage of a two-stage key
        return;
    }
    case Button::Up:
        changeVolume(+5);
        return;
    case Button::Down:
        changeVolume(-5);
        return;
    case Button::Ok:
        if (ev.gesture == Gesture::Long) back();
        else openMenu("Player menu", [this] { return playerMenu(); });
        return;
    case Button::Home:
        if (ev.gesture == Gesture::Long) goHome();
        else back();
        return;
    default:
        return;
    }
}

void App::describeKey(ButtonEvent ev) {
    if (ev.button == Button::Home && ev.gesture == Gesture::Long) {
        keyDescriber_ = false;
        earcons_.play(Earcon::Close);
        say("Key describer off.");
        return;
    }
    say(keyHelp(ev.button) + (ev.button == Button::Home ? " Hold Home now to leave key describer." : ""));
}

// ---------------------------------------------------------------- speech

void App::say(const std::string& text) {
    speechLog_.push_back(text);
    if (speechLog_.size() > kSpeechLogSize) speechLog_.pop_front();
    tts_.say(text);
}

std::string App::itemSpeech(const Screen& screen) const {
    if (screen.items.empty()) return screen.title + ", empty.";
    const MenuItem& item = screen.items[screen.index];
    int verbosity = config_.getInt(kVerbosity, Normal);
    std::string text = item.label;
    if (item.value) text += ", " + item.value();
    if (verbosity <= Normal)
        text += ", " + std::to_string(screen.index + 1) + " of " + std::to_string(screen.items.size());
    if (verbosity == Beginner) {
        if (item.adjust && item.submenu) text += ". Use left and right to change, or press OK for the list.";
        else if (item.adjust) text += ". Use left and right to change.";
        else if (item.submenu) text += ". Press OK to open.";
        else if (item.action) text += ". Press OK to select.";
    }
    return text;
}

std::string App::nowPlayingSpeech() const {
    std::string text = "Now playing. " + book_->title;
    std::string track = chapters()[size_t(currentChapter())].title;
    if (music_ && track != book_->title) text += ", " + track; // a loose track is its own "album"
    return text + (pendingOpen_ ? ", loading." : player_.playing() ? "." : ", paused.");
}

void App::whereAmI() {
    std::string downloading = download_ ? " " + downloadProgress() + "." : "";
    if (mode() == Mode::Player) {
        say(playerStatus() + downloading);
        return;
    }
    std::string path;
    for (const Screen& s : screens_) path += (path.empty() ? "" : ", ") + s.title;
    say(path + ". " + (screens_.empty() ? "" : itemSpeech(screens_.back())) +
        (book_ ? std::string(pendingOpen_ ? ". Loading " : player_.playing() ? ". Playing " : ". Paused, ") +
                     book_->title
               : "") +
        (download_ ? ". " + downloadProgress() + "." : ""));
}

// ---------------------------------------------------------------- screens

void App::openMenu(const std::string& title, std::function<std::vector<MenuItem>()> build, size_t index) {
    Screen screen = menuScreen(title, std::move(build));
    screen.index = std::min(index, screen.items.empty() ? 0 : screen.items.size() - 1);
    pushScreen(std::move(screen));
    earcons_.play(Earcon::Open);
    say(title + ". " + itemSpeech(screens_.back()));
}

void App::pushScreen(Screen screen) {
    if (ping_) server_.cancel(std::exchange(ping_, 0)); // the user moved on from Server status
    cancelResume(); // likewise from a stream resume (resumeStream()) still loading
    screen.id = ++lastScreenId_;
    screens_.push_back(std::move(screen));
}

// Every way off the stack comes here, so a screen's request never outlives it.
void App::closeScreens(size_t keep) {
    while (screens_.size() > keep) {
        std::uint64_t id = screens_.back().id;
        if (auto list = remoteLists_.find(id); list != remoteLists_.end()) {
            if (list->second.request) server_.cancel(list->second.request);
            remoteLists_.erase(list);
        }
        if (ping_ && pingScreen_ == id) server_.cancel(std::exchange(ping_, 0));
        bool drop = pendingOpen_ && pendingOpen_->screen == id;
        screens_.pop_back();
        if (drop) dropPendingOpen(); // after the pop: it may close more screens (leaveEmptyPlayer())
    }
}

Screen* App::findScreen(std::uint64_t id) {
    for (Screen& s : screens_)
        if (s.id == id) return &s;
    return nullptr;
}

void App::activate() {
    Screen& screen = screens_.back();
    if (screen.items.empty()) {
        earcons_.play(Earcon::Edge);
        return;
    }
    MenuItem item = screen.items[screen.index]; // copy: the action may change the screen stack
    if (item.submenu) {
        openMenu(item.label, item.submenu, item.selected ? item.selected() : 0);
    } else if (item.action) {
        item.action();
    } else {
        earcons_.play(Earcon::Edge);
    }
}

// Left / Right only ever change a value. On an item without one they do nothing
// at all, so they can never be mistaken for "back" or "open".
bool App::canAdjust() const {
    const Screen& screen = screens_.back();
    return !screen.items.empty() && screen.items[screen.index].adjust;
}

void App::adjust(int direction) {
    if (!canAdjust()) return;
    MenuItem item = screens_.back().items[screens_.back().index];
    earcons_.play(item.adjust(direction) ? Earcon::Move : Earcon::Edge);
    if (item.value) say(item.value());
}

void App::back() {
    if (screens_.size() <= 1) {
        earcons_.play(Earcon::Edge);
        if (!screens_.empty()) say(screens_.back().title + ". " + itemSpeech(screens_.back()));
        return;
    }
    popScreen();
    earcons_.play(Earcon::Close);
    const Screen& screen = screens_.back();
    say(screen.kind == Screen::Kind::Player ? nowPlayingSpeech() : itemSpeech(screen));
}

void App::popScreen() {
    if (screens_.size() <= 1) return; // Home stays
    closeScreens(screens_.size() - 1);
    Screen& screen = screens_.back();
    if (screen.kind == Screen::Kind::Player || !screen.build) return; // a list from the server keeps what loaded
    // Rebuild, so a list shows what changed meanwhile (e.g. "Now playing" on Home), and stay on the same item.
    std::string label = screen.items.empty() ? "" : screen.items[screen.index].label;
    screen.items = screen.build();
    // Same place if the label is still there (labels may repeat), else its first match, else the nearest index.
    size_t index = std::min(screen.index, screen.items.empty() ? 0 : screen.items.size() - 1);
    if (screen.items.empty() || screen.items[index].label != label) {
        for (size_t i = 0; i < screen.items.size(); ++i) {
            if (screen.items[i].label == label) {
                index = i;
                break;
            }
        }
    }
    screen.index = index;
}

void App::goHome() {
    closeScreens(0);
    openMenu("Home", [this] { return homeMenu(); });
}

// There is only one player screen: return to it if it is on the stack, else push it.
void App::enterPlayer() {
    if (!book_) {
        goHome();
        return;
    }
    if (auto player = playerScreenIndex()) {
        closeScreens(*player + 1);
        earcons_.play(Earcon::Close);
    } else {
        pushScreen(playerScreen());
        earcons_.play(Earcon::Open);
    }
    say(nowPlayingSpeech());
}

std::optional<size_t> App::playerScreenIndex() const {
    auto player = std::find_if(screens_.begin(), screens_.end(),
                               [](const Screen& s) { return s.kind == Screen::Kind::Player; });
    return player == screens_.end() ? std::nullopt : std::optional(size_t(player - screens_.begin()));
}

// After an open the user waited for: the player, unless they have opened
// something else meanwhile (an open is dropped when its screen is left).
void App::enterPlayerFrom(std::uint64_t screenId) {
    if (!screens_.empty() && screens_.back().id == screenId) enterPlayer();
}

// Like the player, there is only one Power menu: holding Power again returns to it, on Sleep.
void App::openPowerMenu() {
    auto power = std::find_if(screens_.begin(), screens_.end(),
                              [](const Screen& s) { return s.kind == Screen::Kind::Menu && s.title == "Power"; });
    if (power == screens_.end()) {
        openMenu("Power", [this] { return powerMenu(); });
        return;
    }
    closeScreens(size_t(power - screens_.begin()) + 1);
    Screen& menu = screens_.back();
    menu.index = 0;
    earcons_.play(Earcon::Close);
    say(menu.title + ". " + itemSpeech(menu));
}

void App::saveChangedSettings() {
    if (settingsChanged_) saveConfig();
}

void App::saveConfig() {
    settingsChanged_ = false;
    volumeChanged_ = false;
    config_.save();
}

// Written once here instead of on every pause, book change and 30 s of playback,
// to spare the flash. A crash or power loss loses the place since the last exit.
void App::saveOnExit() {
    rememberPosition();
    if (settingsChanged_ || volumeChanged_) saveConfig();
    saveState();
    trimCache(true); // the manifests are written on exit too, not at every play (a second call at power off usually finds nothing to do)
}

// Power off writes, and then the front end's shutdown() would write the same again.
void App::saveState() {
    if (state_.values() == savedState_) return;
    if (state_.save()) savedState_ = state_.values(); // else the next exit tries again
}

MenuItem App::choice(const std::string& label, const std::string& key,
                     std::vector<std::pair<int, std::string>> options, int fallback,
                     std::function<void(int)> onChange) {
    // Left / Right step through the options and apply each at once, so the user
    // hears the new speech rate or volume; the file is written when they move on.
    // OK opens the whole list, starting on the current value; choosing one saves it and goes back.
    auto current = [this, key, options, fallback]() -> std::optional<size_t> {
        int v = config_.getInt(key, fallback);
        for (size_t i = 0; i < options.size(); ++i)
            if (options[i].first == v) return i;
        return std::nullopt;
    };
    MenuItem item;
    item.label = label;
    item.value = [this, key, options, fallback, current] {
        auto i = current();
        return i ? options[*i].second : std::to_string(config_.getInt(key, fallback));
    };
    item.adjust = [this, key, options, fallback, onChange, current](int dir) {
        long next = -1;
        if (auto i = current()) {
            next = long(*i) + dir;
        } else { // a stored value that is not an option: step to the nearest option on that side
            int v = config_.getInt(key, fallback);
            for (size_t i = 0; i < options.size(); ++i) {
                int o = options[i].first;
                bool onSide = dir > 0 ? o > v : o < v;
                if (onSide && (next < 0 || (dir > 0 ? o < options[size_t(next)].first : o > options[size_t(next)].first)))
                    next = long(i);
            }
        }
        if (next < 0 || next >= long(options.size())) return false;
        int v = options[size_t(next)].first;
        config_.setInt(key, v);
        settingsChanged_ = true;
        if (onChange) onChange(v);
        return true;
    };
    item.selected = [current] { return current().value_or(0); };
    item.submenu = [this, key, options, onChange, current] {
        std::vector<MenuItem> items;
        auto cur = current();
        for (size_t i = 0; i < options.size(); ++i) {
            int v = options[i].first;
            MenuItem option = action(options[i].second, [this, key, v, onChange] {
                config_.setInt(key, v);
                saveConfig();
                if (onChange) onChange(v);
                back();
            });
            if (cur == i) option.value = [] { return std::string("current"); };
            items.push_back(option);
        }
        return items;
    };
    return item;
}

std::vector<MenuItem> App::homeMenu() {
    std::vector<MenuItem> items;
    if (book_) {
        MenuItem now = action("Now playing", [this] { enterPlayer(); });
        now.value = [this] { return book_ ? book_->title : std::string("nothing"); };
        items.push_back(now);
    } else if (auto stream = savedStream()) {
        // Home is not rebuilt while the resumed track opens: then this goes to the player.
        MenuItem now = action("Now playing", [this] {
            if (book_) enterPlayer();
            else resumeStream();
        });
        now.value = [this, title = stream->title] { return book_ ? book_->title : title; };
        items.push_back(now);
    }
    items.push_back(submenu("Library", [this] { return libraryMenu(); }));
    items.push_back(submenu("Music", [this] { return musicMenu(); }));
    std::string server = config_.getString(kServerName);
    items.push_back(submenu(server.empty() ? "Server" : server, [this] { return streamingMenu(); }));
    items.push_back(submenu("Podcasts", [this] {
        return std::vector<MenuItem>{action("No podcasts yet", [this] { say("Podcast support is coming later."); })};
    }));
    items.push_back(submenu("Settings", [this] { return settingsMenu(); }));
    items.push_back(submenu("System", [this] { return systemMenu(); }));
    items.push_back(submenu("Help", [this] { return helpMenu(); }));
    return items;
}

namespace {
// The items of one kind from the files on this device, in the order of the lists (by title).
std::vector<const IndexItem*> localItems(const LibraryIndex& index, IndexItem::Kind kind) {
    std::vector<const IndexItem*> out;
    for (const auto& [id, item] : index.items())
        if (item.source == IndexItem::Source::Local && item.kind == kind) out.push_back(&item);
    std::stable_sort(out.begin(), out.end(), [](const IndexItem* a, const IndexItem* b) { return naturalLess(a->title, b->title); });
    return out;
}

} // namespace

// The Book a local item plays as: the files in the order of the index, no part titles (the
// player names a local file by its file name). A book keeps its folder (or file) as its path,
// which is the key of its saved place (`pos:`) and `last_book`.
Book App::bookOf(const IndexItem& item) const {
    Book book;
    book.title = item.title;
    for (const IndexTrack& t : item.tracks) {
        book.files.emplace_back(t.location);
        // A download's files are named by the server's track ids: they are spoken by their titles.
        if (item.source == IndexItem::Source::Cached) book.titles.push_back(t.title);
    }
    if (item.id.starts_with(kLocalBookPrefix)) book.path = item.id.substr(std::string(kLocalBookPrefix).size());
    else if (item.id.starts_with(kLocalFolderPrefix)) book.path = item.id.substr(std::string(kLocalFolderPrefix).size());
    else if (!book.files.empty()) book.path = book.files.front().parent_path(); // an album of tags
    return book;
}

std::vector<MenuItem> App::libraryMenu() {
    std::vector<MenuItem> items;
    for (const IndexItem* item : localItems(index_, IndexItem::Kind::Book)) {
        Book b = bookOf(*item);
        MenuItem entry = action(b.title, [this, b] {
            if (openBook(b, true)) enterPlayer();
        });
        if (b.files.size() > 1) {
            size_t n = b.files.size();
            entry.value = [n] { return std::to_string(n) + " parts"; };
        }
        items.push_back(entry);
    }
    if (items.empty() && scanRequest_) // a first start has no index until the scan is done: do not call it empty
        items.push_back(action("Scanning", [this] { say("Looking for books. Try again in a moment."); }));
    else if (items.empty())
        items.push_back(action("Library is empty", [this] {
            say("Copy audio books into " + libraryRoot_.string() + ". One folder per book.");
        }));
    return items;
}

// Music (docs/navigation.md): Play all, Artists, Albums A to Z, Refresh library. Every list is
// built from the index when it is opened, so it is a local menu: no "Loading". An album that
// is on the device (local or downloaded, a server copy merged into it) opens its tracks at
// once; a server-only album loads its tracks like Streaming does. Play all plays what is on
// the device.
std::vector<MenuItem> App::musicMenu() {
    syncDownloads(); // a download made or deleted since the menu was last built
    index_.save();
    std::vector<MergedAlbum> albums = mergeAlbums(index_.items());
    MenuItem refresh = action("Refresh library", [this] { refreshLibrary(); });
    std::vector<MenuItem> items;
    if (albums.empty() && scanRequest_) {
        items.push_back(action("Scanning", [this] { say("Looking for music. Try again in a moment."); }));
        items.push_back(refresh);
        return items;
    }
    if (albums.empty()) {
        items.push_back(action("No music", [this] { say("Copy music into " + musicRoot_.string() + ". One folder per album."); }));
        items.push_back(refresh);
        return items;
    }
    std::stable_sort(albums.begin(), albums.end(),
                     [](const MergedAlbum& a, const MergedAlbum& b) { return naturalLess(a.primary->title, b.primary->title); });
    Book all;
    all.title = "All music";
    all.path = musicRoot_;
    for (const MergedAlbum& m : albums) { // in the order of Albums A to Z
        if (m.primary->source == IndexItem::Source::Server) continue;
        Book b = bookOf(*m.primary);
        all.files.insert(all.files.end(), b.files.begin(), b.files.end());
    }
    MenuItem playAll = action("Play all", [this, all] {
        // Only the server has albums (or the scan has not found the files yet): Play all never plays the server.
        if (all.files.empty()) return say(scanRequest_ ? "Looking for music. Try again in a moment." : "No music");
        if (playMusic(all, 0)) enterPlayer();
    });
    if (!all.files.empty()) playAll.value = [n = all.files.size()] { return countValue(n, "track"); };
    items.push_back(playAll);
    items.push_back(submenu("Artists", [this] { return artistsMenu(); }));
    items.push_back(submenu("Albums A to Z", [this] { return albumList(mergeAlbums(index_.items())); }));
    items.push_back(refresh);
    return items;
}

// The artist of an entry: its primary item's, else the first one any of its items names (a
// local album with no tag that is on the server under an artist is that artist's).
std::string App::artistOf(const MergedAlbum& album) {
    for (const IndexItem* item : album.items) // primary first
        if (!item->artist.empty()) return item->artist;
    return {};
}

// Each artist once (names that differ in case or spacing are one), sorted without an article,
// with the number of albums. Albums with no artist are under "Unknown artist", last.
std::vector<MenuItem> App::artistsMenu() {
    struct Artist {
        std::string name, key;
        size_t albums = 0;
    };
    std::vector<Artist> artists;
    std::map<std::string, size_t> byKey;
    for (const MergedAlbum& m : mergeAlbums(index_.items())) {
        std::string name = artistOf(m);
        std::string key = artistKey(name);
        auto [it, fresh] = byKey.emplace(key, artists.size());
        if (fresh) artists.push_back({name, key, 0});
        ++artists[it->second].albums;
    }
    std::stable_sort(artists.begin(), artists.end(), [](const Artist& a, const Artist& b) {
        if (a.key.empty() != b.key.empty()) return b.key.empty();
        return artistLess(a.name, b.name);
    });
    std::vector<MenuItem> items;
    for (const Artist& a : artists) {
        MenuItem item = submenu(a.key.empty() ? "Unknown artist" : a.name, [this, key = a.key] {
            std::vector<MergedAlbum> theirs;
            for (MergedAlbum& m : mergeAlbums(index_.items()))
                if (artistKey(artistOf(m)) == key) theirs.push_back(std::move(m));
            return albumList(std::move(theirs));
        });
        item.value = [n = a.albums] { return countValue(n, "album"); };
        items.push_back(item);
    }
    return items;
}

// The albums of `albums` by title, as entries of a list.
std::vector<MenuItem> App::albumList(std::vector<MergedAlbum> albums) {
    std::stable_sort(albums.begin(), albums.end(),
                     [](const MergedAlbum& a, const MergedAlbum& b) { return naturalLess(a.primary->title, b.primary->title); });
    std::vector<MenuItem> items;
    for (const MergedAlbum& m : albums) items.push_back(albumEntry(m));
    // An artist's last album may have gone since the list above was built: never an empty list.
    if (items.empty()) items.push_back(action("No albums", [this] { say("There are no albums here any more."); }));
    return items;
}

// One album of a Music list. The closures keep copies: the index may change before OK.
MenuItem App::albumEntry(const MergedAlbum& m) {
    const IndexItem& item = *m.primary;
    std::string info = albumInfoText(m);
    if (item.source == IndexItem::Source::Server) {
        std::string key = catalogueServerKey();
        std::string ref = LibraryIndex::refOnServer(item.seenOn, key).value_or(item.id);
        std::string prefix = LibraryIndex::seenRef(key, SeenKind::Album, ""); // `<key>:album:`
        RemoteAlbum album;
        album.id = ref.starts_with(prefix) ? ref.substr(prefix.size()) : std::string();
        album.name = item.title;
        album.artist = item.artist;
        MenuItem entry = action(item.title, [this, album, info] { openRemoteAlbum(album, info); });
        if (int n = trackCountOf(item); n > 0) entry.value = [n] { return countValue(size_t(n), "track"); };
        return entry;
    }
    Book a = bookOf(item);
    if (a.files.size() == 1 && a.files.front() == a.path) { // a loose track, not a folder
        return action(a.title, [this, a] {
            if (playMusic(a, 0)) enterPlayer();
        });
    }
    MenuItem entry = submenu(a.title, [this, a, info] { return albumMenu(a, info); });
    entry.value = [n = a.files.size()] { return countValue(n, "track"); };
    return entry;
}

// The tracks of an album, then "Album info" (where it plays from), which only reports.
std::vector<MenuItem> App::albumMenu(const Book& album, const std::string& info) {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < album.files.size(); ++i) {
        items.push_back(action(partTitle(album, i), [this, album, i] {
            if (playMusic(album, i)) enterPlayer(); // the rest of the album follows
        }));
    }
    if (!info.empty()) items.push_back(action("Album info", [this, info] { say(info); }));
    return items;
}

// ---------------------------------------------------------------- streaming

std::vector<MenuItem> App::streamingMenu() {
    if (!server_.configured()) {
        if (serverConfigured(config_)) // set up, but not as a server this player can use
            return {action("Server not supported", [this] { say(kUnsupportedServerHelp); })};
        return {action("No server set up", [this] { say(kNoServerHelp); })};
    }
    std::vector<MenuItem> items;
    items.push_back(action("Recently added", [this] {
        openAlbumList("Recently added", "newest", kAlbumPageSize, false);
    }));
    items.push_back(action("Albums A to Z", [this] {
        openAlbumList("Albums A to Z", "alphabeticalByName", kAlbumPageSize, true);
    }));
    items.push_back(action("Artists", [this] {
        auto toItems = [this](const std::vector<RemoteArtist>& artists) {
            std::vector<MenuItem> out;
            for (const RemoteArtist& a : artists) {
                MenuItem item = action(a.name, [this, a] {
                    openRemoteList(a.name,
                                   fetchPages<RemoteAlbum>(
                                       [this, id = a.id](int, MusicServer::Done<RemoteAlbum> done) {
                                           return server_.artistAlbums(id, std::move(done));
                                       },
                                       [this](const std::vector<RemoteAlbum>& albums) { return albumItems(albums, false); }),
                                   0, "No albums", "The server has no albums by " + a.name + ".");
                });
                if (a.albumCount > 0) item.value = [n = size_t(a.albumCount)] { return countValue(n, "album"); };
                out.push_back(item);
            }
            return out;
        };
        openRemoteList("Artists",
                       fetchPages<RemoteArtist>(
                           [this](int, MusicServer::Done<RemoteArtist> done) { return server_.artists(std::move(done)); },
                           toItems),
                       0, "No artists", "The server has no music yet. Add music to the server, then come back.");
    }));
    items.push_back(action("Playlists", [this] {
        auto toItems = [this](const std::vector<RemotePlaylist>& playlists) {
            std::vector<MenuItem> out;
            for (const RemotePlaylist& p : playlists) {
                MenuItem item = action(p.name, [this, p] {
                    openRemoteList(p.name,
                                   fetchPages<RemoteTrack>(
                                       [this, id = p.id](int, MusicServer::Done<RemoteTrack> done) {
                                           return server_.playlistTracks(id, std::move(done));
                                       },
                                       [this, name = p.name, id = p.id](const std::vector<RemoteTrack>& tracks) {
                                           return trackItems(name, tracks, "Play playlist",
                                                             {RemoteSource::Kind::Playlist, id}, "");
                                       }),
                                   0, "No tracks", "This playlist is empty. Add tracks to it on the server.");
                });
                if (p.songCount > 0) item.value = [n = size_t(p.songCount)] { return countValue(n, "track"); };
                out.push_back(item);
            }
            return out;
        };
        openRemoteList("Playlists",
                       fetchPages<RemotePlaylist>(
                           [this](int, MusicServer::Done<RemotePlaylist> done) { return server_.playlists(std::move(done)); },
                           toItems),
                       0, "No playlists", "Make a playlist on the server, then come back.");
    }));
    items.push_back(action("Random albums", [this] {
        openAlbumList("Random albums", "random", kRandomAlbums, false);
    }));
    if (trackCache().enabled()) items.push_back(submenu("Downloaded", [this] { return downloadedMenu(); }));
    items.push_back(action("Server status", [this] { checkServer(); }));
    return items;
}

template <class T>
App::FetchPage App::fetchPages(std::function<MusicServer::RequestId(int offset, MusicServer::Done<T>)> request,
                               std::function<std::vector<MenuItem>(const std::vector<T>&)> toItems) {
    return [request, toItems](int offset, PageDone done) {
        return request(offset, [toItems, done](MusicServer::Error error, std::vector<T> list, bool more) {
            done(error, error == MusicServer::Error::None ? toItems(list) : std::vector<MenuItem>{}, more);
        });
    };
}

// Opens at once with "Loading"; onPage() fills it in. With a page size, the
// following pages are fetched in the background while the server has more.
void App::openRemoteList(const std::string& title, FetchPage fetch, int pageSize, std::string emptyLabel,
                         std::string emptyHelp) {
    pushScreen({Screen::Kind::Menu, title, {}, {}}); // no build: popScreen() keeps what has loaded
    std::uint64_t id = screens_.back().id;
    RemoteList& list = remoteLists_[id];
    list.fetch = std::move(fetch);
    list.pageSize = pageSize;
    list.emptyLabel = std::move(emptyLabel);
    list.emptyHelp = std::move(emptyHelp);
    requestPage(id, 0, true);
    showStatus(id);
    earcons_.play(Earcon::Open);
    say(title + ". " + itemSpeech(screens_.back()));
}

// The callback holds only the screen's id: by the time it runs, the stack may
// have changed. Leaving the screen cancels the request (closeScreens()).
void App::requestPage(std::uint64_t screenId, int offset, bool waiting) {
    auto it = remoteLists_.find(screenId);
    if (it == remoteLists_.end()) return;
    RemoteList& list = it->second;
    list.error = MusicServer::Error::None;
    list.offset = offset;
    list.waiting = waiting;
    list.request = list.fetch(offset, [this, screenId](MusicServer::Error error, std::vector<MenuItem> items, bool more) {
        onPage(screenId, error, std::move(items), more);
    });
}

void App::onPage(std::uint64_t screenId, MusicServer::Error error, std::vector<MenuItem> items, bool more) {
    auto it = remoteLists_.find(screenId);
    Screen* screen = findScreen(screenId);
    if (it == remoteLists_.end() || !screen) return; // its screen is gone (and the request was cancelled)
    RemoteList& list = it->second;
    list.request = 0;
    size_t first = list.loaded;
    // The user is on "Loading" (not on an item that has loaded): move them to what came.
    bool onLoading = list.waiting && screen->index >= first;
    list.waiting = false;
    if (error != MusicServer::Error::None) {
        list.error = error;
    } else {
        size_t count = items.size();
        screen->items.insert(screen->items.begin() + std::ptrdiff_t(list.loaded), std::make_move_iterator(items.begin()),
                             std::make_move_iterator(items.end()));
        list.loaded += count;
        // The next page starts after what came, whatever size the server sends.
        if (list.pageSize > 0 && more && count > 0) requestPage(screenId, list.offset + int(count), false);
    }
    showStatus(screenId);
    if (onLoading) screen->index = std::min(first, screen->items.size() - 1);
    // Filled silently when the user has moved on to another screen, or sleeps.
    if (!onLoading || &screens_.back() != screen || sleeping_) return;
    tts_.stop();
    if (error != MusicServer::Error::None) earcons_.play(Earcon::Error);
    say(itemSpeech(*screen));
}

// After what has loaded comes one of: the error and "Try again", "Loading", or
// why the list is empty. So the list is never empty.
void App::showStatus(std::uint64_t screenId) {
    auto it = remoteLists_.find(screenId);
    Screen* screen = findScreen(screenId);
    if (it == remoteLists_.end() || !screen) return;
    const RemoteList& list = it->second;
    std::vector<MenuItem>& items = screen->items;
    items.erase(items.begin() + std::ptrdiff_t(list.loaded), items.end()); // the old status
    if (list.error != MusicServer::Error::None) {
        MusicServer::Error error = list.error;
        items.push_back(action(errorText(error), [this, error] { say(errorHelp(error)); }));
        items.push_back(action("Try again", [this, screenId] {
            auto again = remoteLists_.find(screenId);
            Screen* screen = findScreen(screenId);
            if (again == remoteLists_.end() || !screen) return;
            requestPage(screenId, again->second.offset, true);
            showStatus(screenId);
            screen->index = screen->items.size() - 1; // on "Loading"
            say(itemSpeech(*screen));
        }));
        // Downloads need no server: offered when there are some (the same rule as Streaming).
        if (hasDownloads()) items.push_back(submenu("Downloaded", [this] { return downloadedMenu(); }));
    } else if (list.waiting) {
        items.push_back(action("Loading", [this] { say("Still loading."); }));
    } else if (list.loaded == 0) {
        items.push_back(action(list.emptyLabel, [this, help = list.emptyHelp] { say(help); }));
    }
    screen->index = std::min(screen->index, items.size() - 1);
}

void App::openAlbumList(const std::string& title, const std::string& listType, int pageSize, bool paged) {
    openRemoteList(title,
                   fetchPages<RemoteAlbum>(
                       [this, listType, pageSize](int offset, MusicServer::Done<RemoteAlbum> done) {
                           return server_.albums(listType, offset, pageSize, std::move(done));
                       },
                       [this](const std::vector<RemoteAlbum>& albums) { return albumItems(albums, true); }),
                   paged ? pageSize : 0, "No albums", "The server has no music yet. Add music to the server, then come back.");
}

void App::openRemoteAlbum(const RemoteAlbum& album, const std::string& info) {
    openRemoteList(album.name,
                   fetchPages<RemoteTrack>(
                       [this, id = album.id](int, MusicServer::Done<RemoteTrack> done) {
                           return server_.albumTracks(id, std::move(done));
                       },
                       [this, name = album.name, artist = album.artist, id = album.id, info](const std::vector<RemoteTrack>& tracks) {
                           std::vector<MenuItem> items = trackItems(name, tracks, "Play album", {RemoteSource::Kind::Album, id}, artist);
                           if (!items.empty() && !info.empty()) items.push_back(action("Album info", [this, info] { say(info); }));
                           return items;
                       }),
                   0, "No tracks", "This album has no tracks on the server.");
}

// `withArtist`: lists of many artists say whose album it is; one artist's list says how long.
std::vector<MenuItem> App::albumItems(const std::vector<RemoteAlbum>& albums, bool withArtist) {
    std::vector<MenuItem> items;
    for (const RemoteAlbum& a : albums) {
        MenuItem item = action(a.name, [this, a] { openRemoteAlbum(a); });
        if (withArtist && !a.artist.empty()) item.value = [artist = a.artist] { return artist; };
        else if (!withArtist && a.songCount > 0) item.value = [n = size_t(a.songCount)] { return countValue(n, "track"); };
        items.push_back(item);
    }
    return items;
}

// "Play album" first, then "Download album" where downloads can work, then the
// tracks: OK on one plays from it to the end, like Music.
std::vector<MenuItem> App::trackItems(const std::string& title, const std::vector<RemoteTrack>& tracks,
                                      const std::string& playAll, const RemoteSource& source,
                                      const std::string& artist, std::optional<MenuItem> second) {
    if (tracks.empty()) return {};
    auto list = std::make_shared<const std::vector<RemoteTrack>>(tracks); // one copy for all the items
    std::vector<MenuItem> items;
    MenuItem all = action(playAll, [this, title, list, source, artist] { playRemote(title, *list, 0, source, 0, artist); });
    all.value = [n = tracks.size()] { return countValue(n, "track"); };
    items.push_back(all);
    if (second) {
        items.push_back(std::move(*second));
    } else if (canDownload()) {
        const char* what = source.kind == RemoteSource::Kind::Album ? "Download album" : "Download playlist";
        items.push_back(action(what, [this, title, artist, list, source] { startDownload(title, artist, *list, source); }));
    }
    for (size_t i = 0; i < tracks.size(); ++i) {
        std::string label = tracks[i].title.empty() ? "Track " + std::to_string(i + 1) : tracks[i].title;
        items.push_back(action(label, [this, title, list, i, source, artist] { playRemote(title, *list, i, source, 0, artist); }));
    }
    return items;
}

// ---------------------------------------------------------- downloaded lists

namespace {
// What a downloaded list is called in a menu; a list saved without a name is still spoken as what it is.
std::string downloadedName(const CachedList& list) {
    if (!list.name.empty()) return list.name;
    return list.source.kind == RemoteSource::Kind::Album ? "Album" : "Playlist";
}

bool sameList(const RemoteSource& a, const RemoteSource& b) { return a.kind == b.kind && a.id == b.id; }
} // namespace

// The tracks of `list` whose files are whole on disk, in manifest order.
std::vector<RemoteTrack> App::completeTracks(const TrackCache& store, const RemoteSource& source, const CachedList& list) {
    std::vector<RemoteTrack> tracks;
    for (const CachedTrack& t : list.tracks) {
        if (!store.completeFile(source, t)) continue;
        RemoteTrack track;
        track.id = t.id;
        track.title = t.title;
        track.duration = t.duration;
        tracks.push_back(std::move(track));
    }
    return tracks;
}

bool App::hasDownloads() const {
    const TrackCache& store = trackCache();
    return store.enabled() && !store.lists(true).empty();
}

// Streaming > Downloaded: the lists whose manifests are in the cache, by name.
// Read from disk each time the menu is built, so it shows what changed meanwhile.
std::vector<MenuItem> App::downloadedMenu() {
    std::vector<MenuItem> items;
    for (const CachedList& list : trackCache().lists(true)) { // what the user downloaded, not the automatic cache
        MenuItem item = submenu(downloadedName(list), [this, source = list.source] { return downloadedListMenu(source); });
        std::string about = list.source.kind == RemoteSource::Kind::Playlist ? "playlist" : list.artist;
        if (!about.empty()) item.value = [about] { return about; };
        items.push_back(std::move(item));
    }
    if (items.empty())
        items.push_back(action("No downloads", [this] { say("Nothing is downloaded. Choose Download album in an album."); }));
    return items;
}

// The tracks of a downloaded list that are whole on disk: "Play album" first, then
// "Delete download", then the tracks (trackItems(), with Delete in place of Download). They play from their files like any list.
std::vector<MenuItem> App::downloadedListMenu(const RemoteSource& source) {
    const TrackCache& store = trackCache();
    CachedList list = store.load(source);
    std::vector<RemoteTrack> tracks = completeTracks(store, source, list);
    if (tracks.empty()) return {action("No tracks", [this] { say("The tracks of this download are gone from the disk."); })};
    std::string title = downloadedName(list);
    // A dangerous action: behind a list that opens on Cancel.
    MenuItem del = submenu("Delete download", [this, source, title] {
        return std::vector<MenuItem>{
            action("Cancel", [this] { back(); }),
            action("Yes, delete", [this, source, title] { deleteDownload(source, title); }),
        };
    });
    return trackItems(title, tracks, source.kind == RemoteSource::Kind::Album ? "Play album" : "Play playlist", source,
                      list.artist, std::move(del));
}

// Removes the list's folder. The list that is loaded in the player is refused (its
// next tracks would have to come from the network); a download of it is cancelled first.
void App::deleteDownload(const RemoteSource& source, const std::string& title) {
    if (book_ && book_->remote && book_->source && sameList(*book_->source, source)) {
        earcons_.play(Earcon::Error);
        say("Cannot delete " + title + " while it is loaded in the player. Play something else first.");
        return;
    }
    if (download_ && sameList(download_->source, source)) {
        downloader_.cancel(download_->request);
        download_.reset();
    }
    dropAutoCache(source);
    if (!trackCache().remove(source)) {
        earcons_.play(Earcon::Error);
        say("Cannot delete " + title + ".");
        return;
    }
    cacheIndex_.reset(); // its files are gone: the index is made again from the manifests left
    popScreen();         // the confirmation, then the list that is gone, back to Downloaded (rebuilt)
    popScreen();
    earcons_.play(Earcon::Close);
    say("Deleted " + title + ". " + itemSpeech(screens_.back()));
}

// Says "Checking" now and the answer when it comes, if the user is still on
// this screen. Leaving the screen cancels it.
void App::checkServer() {
    if (ping_) server_.cancel(std::exchange(ping_, 0));
    std::uint64_t screenId = screens_.back().id;
    pingScreen_ = screenId;
    say("Checking the server.");
    ping_ = server_.ping([this, screenId](MusicServer::Error error, MusicServer::ServerInfo info) {
        ping_ = 0;
        if (sleeping_ || screens_.empty() || screens_.back().id != screenId) return; // the user has moved on
        tts_.stop();
        if (error != MusicServer::Error::None) {
            earcons_.play(Earcon::Error);
            say(errorText(error) + ".");
            return;
        }
        say(connectedText(info));
    });
}

std::optional<App::StreamPlace> App::savedStream() const {
    if (!server_.configured()) return std::nullopt;
    std::string url = serverIdentity(config_.getString(kServerUrl));
    if (url.empty() || serverIdentity(state_.getString(kLastStreamServer)) != url ||
        state_.getString(kLastStreamUser) != config_.getString(kServerUser))
        return std::nullopt; // another server, or none: its ids mean nothing here
    StreamPlace place;
    std::string kind = state_.getString(kLastStreamKind);
    if (kind == "album") place.source.kind = RemoteSource::Kind::Album;
    else if (kind == "playlist") place.source.kind = RemoteSource::Kind::Playlist;
    else return std::nullopt;
    place.source.id = state_.getString(kLastStreamId);
    place.title = state_.getString(kLastStreamTitle);
    place.trackId = state_.getString(kLastStreamTrackId);
    if (place.source.id.empty()) return std::nullopt;
    try {
        place.track = std::stoul(state_.getString(kLastStreamTrack, "0"));
        place.position = std::stod(state_.getString(kLastStreamPos, "0"));
    } catch (...) {
        place.track = 0, place.position = 0;
    }
    if (!std::isfinite(place.position) || place.position < 0) place.position = 0;
    return place;
}

// The track of a resumed list: found by its id; if it is gone, the track now at
// its index plays from its start, or the first one.
void App::playResumed(const StreamPlace& place, const std::vector<RemoteTrack>& tracks) {
    size_t index = place.track < tracks.size() ? place.track : 0;
    double position = 0;
    auto same = [&](size_t i) { return i < tracks.size() && tracks[i].id == place.trackId; };
    if (!same(place.track)) {
        for (size_t i = 0; i < tracks.size(); ++i)
            if (same(i)) {
                index = i;
                break;
            }
    }
    if (same(index)) position = place.position;
    playRemote(place.title, tracks, index, place.source, position);
}

// Where the list comes from: a list the user downloaded whole, with the saved track
// among it, plays from the disk at once, with no "Loading" and no question to the
// server (offline, a question waits up to the connect timeout). Otherwise the list is
// loaded again rather than saved: it may have changed on the server, and stream URLs
// (with a fresh salt each) are never kept. If the server cannot answer, the tracks
// of it that are on disk play instead (a streamed album kept in part), else the
// error is spoken.
void App::resumeStream() {
    auto place = savedStream();
    if (!place) return;
    cancelResume();
    const TrackCache& store = trackCache();
    CachedList cached = store.enabled() ? store.load(place->source) : CachedList{};
    std::vector<RemoteTrack> onDisk = completeTracks(store, place->source, cached);
    bool wholeDownload = !cached.tracks.empty() && onDisk.size() == cached.tracks.size() &&
                         std::all_of(cached.tracks.begin(), cached.tracks.end(), [](const CachedTrack& t) { return t.pinned; });
    bool hasSaved = std::any_of(onDisk.begin(), onDisk.end(), [&](const RemoteTrack& t) { return t.id == place->trackId; });
    if (wholeDownload && hasSaved) {
        playResumed(*place, onDisk);
        return;
    }
    say("Loading.");
    auto done = [this, place = *place, onDisk = std::move(onDisk), hasSaved](MusicServer::Error error,
                                                                              std::vector<RemoteTrack> tracks, bool) {
        resume_ = 0; // a new screen or sleep would have cancelled it: the user is still where they asked
        tts_.stop();
        if (error != MusicServer::Error::None) {
            if (!onDisk.empty()) { // the server is down, but part of the list is here
                StreamPlace here = place;
                if (!hasSaved) here.track = 0, here.trackId.clear(); // the saved track is not here: start at the first
                playResumed(here, onDisk);
                return;
            }
            earcons_.play(Earcon::Error);
            say(errorText(error) + ".");
            return;
        }
        playResumed(place, tracks);
    };
    resume_ = place->source.kind == RemoteSource::Kind::Album ? server_.albumTracks(place->source.id, done)
                                                               : server_.playlistTracks(place->source.id, done);
}

void App::cancelResume() {
    if (resume_) server_.cancel(std::exchange(resume_, 0));
}

// The entry of sleepTimerMenu() that is set now.
size_t App::sleepTimerIndex() const {
    if (sleepAtChapterEnd_ >= 0) return std::size(kSleepTimes);
    for (size_t i = 0; i < std::size(kSleepTimes); ++i)
        if (kSleepTimes[i].minutes == (sleepDeadline_ ? sleepMinutes_ : 0)) return i;
    return 0;
}

std::vector<MenuItem> App::sleepTimerMenu() {
    auto set = [this](std::string label, int minutes, bool endOfChapter) {
        return action(std::move(label), [this, minutes, endOfChapter] {
            setSleepTimer(minutes, endOfChapter);
            back();
        });
    };
    std::vector<MenuItem> items;
    for (const SleepTime& t : kSleepTimes) items.push_back(set(t.label, t.minutes, false));
    // With nothing loaded there is no chapter to wait for.
    if (book_) items.push_back(set("End of " + unit(), 0, true));
    if (size_t current = sleepTimerIndex(); current < items.size())
        items[current].value = [] { return std::string("current"); };
    return items;
}

std::vector<MenuItem> App::settingsMenu() {
    std::vector<MenuItem> items;
    items.push_back(choice("Speech rate", kSpeechRate, range(100, 460, 30, " words per minute"), 220,
                           [this](int v) { tts_.setRate(v); }));
    items.push_back(choice("Volume", kVolume, range(0, 100, 5, " percent"), 70,
                           [this](int v) { volume_.setVolume(v / 100.0f); }));
    items.push_back(choice("Verbosity", kVerbosity, {{Beginner, "beginner"}, {Normal, "normal"}, {Expert, "expert"}}, Normal));
    items.push_back(choice("Skip back", kSkipBack, seconds({5, 10, 15, 30, 60}), 10));
    items.push_back(choice("Skip forward", kSkipForward, seconds({5, 10, 15, 30, 60, 120}), 30));
    items.push_back(choice("Rewind on resume", kRewindOnResume, seconds({0, 2, 5, 10}), 2));
    items.push_back(choice("Long press time", kLongPress, range(300, 1500, 100, " milliseconds"), 600,
                           [this](int v) { gestures_.setLongPress(std::chrono::milliseconds(v)); }));
    items.push_back(choice("Menu wrap around", kMenuWrap, {{0, "off"}, {1, "on"}}, 1));
    items.push_back(choice("Auto sleep", kAutoSleep,
                           {{1, "1 minute"}, {2, "2 minutes"}, {5, "5 minutes"}, {10, "10 minutes"}, {30, "30 minutes"}, {0, "never"}}, 5));
    // Read when a track is opened: a change takes effect with the next one.
    items.push_back(choice("Streaming quality", kStreamingBitrate, kStreamingQualities, kOriginalQuality));
    // Read by the automatic cache when it queues a track; a lower limit evicts on the next exit or cached track.
    items.push_back(choice("Cache size", kCacheLimit,
                           {{0, "off"}, {1, "1 gigabyte"}, {4, "4 gigabytes"}, {16, "16 gigabytes"}},
                           kDefaultCacheGigabytes));
    MenuItem timer = submenu("Sleep timer", [this] { return sleepTimerMenu(); });
    timer.selected = [this] { return sleepTimerIndex(); };
    timer.value = [this] {
        if (sleepDeadline_) return spokenDuration(std::chrono::duration<double>(*sleepDeadline_ - clock_.now()).count()) + " left";
        return sleepAtChapterEnd_ >= 0 ? "end of " + unit() : std::string("off");
    };
    items.push_back(timer);
    return items;
}

std::vector<MenuItem> App::systemMenu() {
    std::vector<MenuItem> items;
    items.push_back(action("Status", [this] { say(statusReport()); }));
    items.push_back(action("Time and date", [this] {
        char buf[64];
        std::time_t t = clock_.wallTime();
        std::strftime(buf, sizeof buf, "It is %H:%M, %A %d %B.", std::localtime(&t));
        say(buf);
    }));
    items.push_back(submenu("Wi-Fi", [this] {
        return std::vector<MenuItem>{
            action("Status", [this] { say("Connected to Simulated network. Signal good."); }),
            action("Scan for networks", [this] { say("Found 2 networks: Home, Cafe. Joining needs voice input, coming later."); }),
            action("Turn off Wi-Fi", [this] { say("Wi-Fi control is simulated."); }),
        };
    }));
    items.push_back(action("Sync", [this] { say("Sync server not configured."); }));
    items.push_back(action("Software update", [this] { say(std::string("Software is up to date. Version ") + kVersion + "."); }));
    items.push_back(action("About", [this] { say(std::string("Open Audio Player, version ") + kVersion + ". Simulator."); }));
    items.push_back(submenu("Factory reset", [this] {
        return std::vector<MenuItem>{
            action("Cancel", [this] { back(); }),
            action("Erase all settings", [this] { factoryReset(); }),
        };
    }));
    items.push_back(submenu("Power", [this] { return powerMenu(); }));
    return items;
}

std::vector<MenuItem> App::powerMenu() {
    return {
        action("Sleep", [this] {
            popScreen(); // wake up where you were, not in the Power menu
            sleep("Going to sleep.");
        }),
        action("Power off", [this] { powerOff(); }),
        action("Restart", [this] { restart(); }),
        action("Cancel", [this] { back(); }),
    };
}

std::vector<MenuItem> App::helpMenu() {
    std::vector<MenuItem> items;
    items.push_back(action("Getting started", [this] {
        say("This player has no screen. Up and Down move through a menu. OK opens the item, Home goes back, "
            "and holding Home goes to the home menu. Left and Right change a setting. Play pause starts and "
            "stops playback, and holding it tells you where you are. Power puts the player to sleep.");
    }));
    items.push_back(submenu("Buttons", [this] {
        std::vector<MenuItem> buttons;
        for (const ButtonHelp& h : kButtonHelp) {
            Button b = h.button;
            buttons.push_back(action(h.name, [this, b] { say(keyHelp(b)); }));
        }
        return buttons;
    }));
    items.push_back(action("Key describer", [this] {
        keyDescriber_ = true;
        earcons_.play(Earcon::Open);
        say("Key describer on. Press any key to hear what it does. Hold Home to leave.");
    }));
    return items;
}

std::vector<MenuItem> App::playerMenu() {
    std::vector<MenuItem> items;
    MenuItem parts = submenu(music_ ? "Tracks" : "Chapters", [this] { return chaptersMenu(); });
    parts.selected = [this] { return size_t(currentChapter()); };
    items.push_back(parts);
    MenuItem timer = submenu("Sleep timer", [this] { return sleepTimerMenu(); });
    timer.selected = [this] { return sleepTimerIndex(); };
    items.push_back(timer);
    items.push_back(action(music_ ? "Album info" : "Book info", [this] {
        if (!book_) return;
        auto ch = chapters();
        say(book_->title + ". " + std::to_string(ch.size()) + " " + unit() + "s. " + playerStatus());
    }));
    items.push_back(action(music_ ? "Go to first track" : "Go to start of book", [this] {
        // Once it is open; if it fails, the error is said and the user stays in the menu.
        loadFile(0, 0, willPlay(), Origin::Menu, [this, screen = screens_.back().id] { enterPlayerFrom(screen); });
    }));
    items.push_back(action("Speed", [this] { say("Speed control is coming later."); }));
    items.push_back(action("Add bookmark", [this] { say("Bookmarks are coming later."); }));
    return items;
}

std::vector<MenuItem> App::chaptersMenu() {
    std::vector<MenuItem> items;
    auto list = chapters();
    for (size_t i = 0; i < list.size(); ++i) {
        items.push_back(action(list[i].title, [this, i] {
            // Once it is open; if it fails, the error is said and the user stays in the list.
            gotoChapter(int(i), Origin::Menu, [this, screen = screens_.back().id] { enterPlayerFrom(screen); });
        }));
    }
    return items;
}

// ---------------------------------------------------------------- playback

bool App::openBook(const Book& book, bool autoplay) {
    if (book_) rememberPosition();
    size_t file = 0;
    double pos = 0;
    std::string saved = state_.getString("pos:" + book.path.string());
    if (auto colon = saved.find(':'); colon != std::string::npos) {
        try {
            file = std::stoul(saved.substr(0, colon));
            pos = std::stod(saved.substr(colon + 1));
        } catch (...) {
            file = 0;
            pos = 0;
        }
        if (file >= book.files.size()) file = 0, pos = 0;
    }
    if (switchTo(book, false, file, pos, autoplay) == OpenResult::Failed) return false; // a local book opens at once
    state_.setString(kLastBook, book.path.string());
    state_.clearPrefix(kLastStream); // the book played last: start() resumes it, not the stream
    sleepAtChapterEnd_ = -1;
    return true;
}

// Music plays from the chosen track. Local music is never resumed later, so it
// neither saves a position nor replaces the book that start() resumes; a remote
// list keeps its place as last_stream (rememberPosition()).
bool App::playMusic(const Book& album, size_t track, Then opened, double position) {
    if (book_) rememberPosition();
    // Only once it plays: if it fails, the book before it keeps its sleep timer and rewind.
    auto playing = [this, opened = std::move(opened)] {
        sleepAtChapterEnd_ = -1;
        pausedAt_.reset();
        if (opened) opened();
    };
    return switchTo(album, true, track, position, true, std::move(playing)) != OpenResult::Failed;
}

bool App::playRemote(const std::string& title, const std::vector<RemoteTrack>& tracks, size_t index,
                     std::optional<RemoteSource> source, double position, const std::string& artist) {
    if (index >= tracks.size()) {
        earcons_.play(Earcon::Error);
        say("Nothing to play.");
        return false;
    }
    Book list;
    list.title = title;
    list.remote = true;
    list.source = std::move(source);
    list.artist = artist;
    for (const RemoteTrack& t : tracks) {
        list.suffixes.push_back(t.suffix);
        list.sizes.push_back(t.size);
        list.files.emplace_back(t.id);
        list.titles.push_back(t.title);
        list.durations.push_back(t.duration);
    }
    std::uint64_t screen = screens_.empty() ? 0 : screens_.back().id;
    return playMusic(list, index, [this, screen] { enterPlayerFrom(screen); }, position);
}

// Loads `book` from `file`. If none of its files from there on opens, what was
// loaded before stays loaded, at the same place, so the user is where they were.
App::OpenResult App::switchTo(const Book& book, bool music, size_t file, double position, bool autoplay,
                              Then opened) {
    dropPendingOpen(); // a failure goes back to what was loaded before it
    size_t wasFile = fileIndex_;
    bool wasOpen = player_.isOpen(), wasPlaying = player_.playing();
    double wasAt = player_.position();
    auto undo = [this, previous = std::exchange(book_, book), wasMusic = std::exchange(music_, music), wasFile,
                 wasOpen, wasPlaying, wasAt, remote = book.remote] {
        book_ = previous;
        music_ = wasMusic;
        fileIndex_ = wasFile;
        if (!book_ || !wasOpen) return;
        if (book_->remote && remote) {
            reopenAt_ = wasAt; // its server is likely gone too: open it on Play, not now
        } else if (book_->remote) {
            // A local book failed: the server had nothing to do with it. The track opens
            // again in the background; if that fails, Play tries it again.
            openFile(wasFile, wasAt, wasPlaying, Origin::Restore, {}, [this, wasAt] { reopenAt_ = wasAt; });
        } else if (player_.open(book_->files[wasFile].string(), wasAt)) {
            if (wasPlaying && !remote) player_.play(); // the server is gone: stay paused
        } else {
            book_.reset(), music_ = false;
        }
    };
    return openFile(file, position, autoplay, Origin::Menu, std::move(opened), std::move(undo));
}

// Moves to `file` of the loaded book. If a remote track does not open, the
// track that was loaded stays the current one, paused at the same place, and
// then `undo` runs.
App::OpenResult App::loadFile(size_t file, double position, bool autoplay, Origin origin, Then opened,
                              Then undo) {
    // A track still opening is of this book too (`file` is one of its tracks): it
    // is dropped, but not undone now, since its undo may load another book (that
    // of switchTo()). It runs if this open fails, after this one's own.
    Then earlier;
    if (pendingOpen_) {
        earlier = std::move(pendingOpen_->undo);
        pendingOpen_.reset();
        player_.close();
    }
    size_t wasFile = fileIndex_;
    bool wasOpen = player_.isOpen();
    double wasAt = player_.position();
    auto restore = [this, wasFile, wasOpen, wasAt, undo = std::move(undo), earlier = std::move(earlier)] {
        if (book_ && book_->remote) {
            fileIndex_ = wasFile;
            if (wasOpen) reopenAt_ = wasAt; // opened on Play: not a second wait on the server now
        }
        if (undo) undo();
        if (earlier) earlier();
    };
    return openFile(file, position, autoplay, origin, std::move(opened), std::move(restore));
}

// Opens `file`, or the next one that opens: a file that cannot be read is
// reported and skipped, so one bad file does not stop a book or an album. A
// remote track is tried once: when the server cannot be reached, every
// following track would wait on it too. A local file opens here; a remote one
// may still be opening on return (Pending), and finishPendingOpen() goes on.
App::OpenResult App::openFile(size_t file, double position, bool autoplay, Origin origin, Then opened,
                              Then undo) {
    if (!book_) return OpenResult::Failed;
    for (; file < book_->files.size(); ++file, position = 0) {
        fileIndex_ = file;
        bool opens = false;
        if (book_->remote) {
            // A downloaded track is a local file: it opens at once and seeks by itself. If it
            // cannot be opened after all, the stream is the way.
            if (std::optional<std::filesystem::path> cached = cachedFile(book_->files[file].string(), true)) {
                opens = player_.open(cached->string(), position);
                if (!opens && cacheIndex_) cacheIndex_->erase(book_->files[file].string()); // not tried again at every play
            }
            if (!opens) {
                // A stream takes a moment to open: say why nothing plays yet.
                if (origin == Origin::Menu || origin == Origin::Key || origin == Origin::Resume) say("Loading.");
                MusicServer::Stream stream = remoteStream(file);
                opens = player_.open(stream.url, position, stream.transcoded);
            }
        } else {
            opens = player_.open(book_->files[file].string(), position);
        }
        if (opens) {
            if (player_.opening()) {
                std::uint64_t screen = origin == Origin::Menu && !screens_.empty() ? screens_.back().id : 0;
                pendingOpen_ = PendingOpen{screen,    origin,         partTitle(*book_, file), autoplay,
                                           position,  clock_.now(),   std::move(opened),       std::move(undo)};
                return OpenResult::Pending;
            }
            onOpened(autoplay, origin, opened);
            return OpenResult::Opened;
        }
        if (book_->remote) {
            onCannotPlay(partTitle(*book_, file), origin == Origin::Restore, undo);
            return OpenResult::Failed;
        }
        earcons_.play(Earcon::Error);
        say("Cannot open " + book_->files[file].filename().string());
    }
    if (undo) undo();
    return OpenResult::Failed;
}

void App::onOpened(bool autoplay, Origin origin, const Then& opened) {
    reopenAt_.reset();
    // The current track opened again (Play after it broke off, or it is back
    // behind a choice that failed) goes on with its play.
    bool goesOn = scrobble_ && (origin == Origin::Resume || origin == Origin::Restore);
    if (!book_->remote) scrobble_.reset();
    else if (!goesOn) startTrackPlay();
    if (autoplay) player_.play();
    if (opened) opened();
}

void App::onCannotPlay(const std::string& title, bool quiet, const Then& undo) {
    if (!quiet) {
        earcons_.play(Earcon::Error);
        say("Cannot play " + title + ". Server not reachable."); // never the URL: it holds the login
    }
    if (undo) undo();
    leaveEmptyPlayer();
}

// After an undo: nothing may be loaded now (a resumed list, whose player the
// user entered while it opened). Then there is no player to stay in.
void App::leaveEmptyPlayer() {
    if (auto player = playerScreenIndex(); !book_ && player) closeScreens(*player);
}

void App::finishPendingOpen() {
    PendingOpen open = std::move(*pendingOpen_);
    pendingOpen_.reset(); // before the callbacks, which may open again
    if (player_.isOpen()) onOpened(open.autoplay, open.origin, open.opened);
    else onCannotPlay(open.title, open.origin == Origin::Restore, open.undo);
}

void App::giveUpPendingOpen() {
    PendingOpen open = std::move(*pendingOpen_);
    pendingOpen_.reset();
    player_.close(); // drops the open: its result never shows
    onCannotPlay(open.title, open.origin == Origin::Restore, open.undo);
}

// The user has moved on before the track opened: no result of it is ever heard.
void App::dropPendingOpen() {
    if (!pendingOpen_) return;
    Then undo = std::move(pendingOpen_->undo);
    pendingOpen_.reset();
    player_.close();
    if (undo) undo();
    leaveEmptyPlayer();
}

bool App::willPlay() const { return player_.playing() || (pendingOpen_ && pendingOpen_->autoplay); }

void App::togglePlay() {
    if (pendingOpen_) { // whether it plays once it has opened
        pendingOpen_->autoplay = !pendingOpen_->autoplay;
        say(pendingOpen_->autoplay ? "Loading." : "Paused.");
    } else if (player_.playing()) {
        pause(true);
    } else {
        play();
    }
}

void App::play() {
    int rewind = config_.getInt(kRewindOnResume, 2);
    bool rewinds = rewind > 0 && pausedAt_ && clock_.now() - *pausedAt_ > std::chrono::seconds(10);
    if (book_ && !player_.isOpen() && reopenAt_) {
        pausedAt_.reset();
        double at = std::max(0.0, *reopenAt_ - (rewinds ? rewind : 0));
        // Or it says "Server not reachable", and Play tries again at the same place.
        openFile(fileIndex_, at, true, Origin::Resume, [this] { if (player_.playing()) earcons_.play(Earcon::Play); }, {});
        return;
    }
    if (!book_ && savedStream()) { // after a restart: like "Now playing" on Home
        resumeStream();
        return;
    }
    if (!book_ || !player_.isOpen()) {
        earcons_.play(Earcon::Error);
        say("Nothing to play. Choose a book in the library.");
        return;
    }
    if (rewinds) player_.seek(player_.position() - rewind);
    pausedAt_.reset();
    player_.play();
    earcons_.play(Earcon::Play);
}

void App::pause(bool announce) {
    if (!player_.playing()) return;
    player_.pause();
    pausedAt_ = clock_.now();
    rememberPosition();
    if (announce) say("Paused.");
}

void App::skip(int direction) {
    if (!player_.isOpen()) {
        earcons_.play(Earcon::Edge);
        return;
    }
    int amount = direction < 0 ? config_.getInt(kSkipBack, 10) : config_.getInt(kSkipForward, 30);
    double target = player_.position() + direction * amount;
    if (target < 0 || (player_.duration() > 0 && target >= player_.duration())) {
        earcons_.play(Earcon::Edge);
        target = std::clamp(target, 0.0, std::max(0.0, player_.duration() - 1));
    } else {
        earcons_.play(Earcon::Move);
    }
    player_.seek(target);
    if (!player_.playing())
        say((direction < 0 ? "Back " : "Forward ") + spokenDuration(amount) + ".");
}

void App::changeVolume(int delta) {
    int v = std::clamp(config_.getInt(kVolume, 70) + delta, 0, 100);
    config_.setInt(kVolume, v);
    volumeChanged_ = true; // a config.yaml write per press would wear the flash
    volume_.setVolume(v / 100.0f);
    earcons_.play(Earcon::Volume);
    if (v == 100) say("Maximum volume.");
    else if (v == 0) say("Muted.");
    else if (!player_.playing()) say("Volume " + std::to_string(v) + " percent.");
}

std::vector<App::ChapterRef> App::chapters() const {
    std::vector<ChapterRef> out;
    if (!book_) return out;
    if (book_->files.size() > 1) {
        for (size_t i = 0; i < book_->files.size(); ++i)
            out.push_back({partTitle(*book_, i), i, 0});
    } else if (!player_.chapters().empty()) {
        for (const Chapter& c : player_.chapters()) out.push_back({c.title, 0, c.start});
    } else {
        out.push_back({book_->remote ? partTitle(*book_, 0) : book_->title, 0, 0});
    }
    return out;
}

int App::currentChapter() const {
    if (!book_) return 0;
    if (book_->files.size() > 1) return int(fileIndex_);
    double pos = player_.position() + 0.25;
    int cur = 0;
    const auto& list = player_.chapters();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].start <= pos) cur = int(i);
    return cur;
}

// `opened` runs once the chapter's file is open: at once, unless it is a remote track.
bool App::gotoChapter(int index, Origin origin, Then opened) {
    auto list = chapters();
    if (index < 0 || index >= int(list.size())) return false;
    const ChapterRef& ch = list[size_t(index)];
    auto arrived = [this, index, opened = std::move(opened)] {
        if (sleepAtChapterEnd_ >= 0) sleepAtChapterEnd_ = index;
        if (opened) opened();
    };
    // Closed: a remote track that failed, ended the list or is still opening.
    if (ch.file != fileIndex_ || !player_.isOpen())
        return loadFile(ch.file, ch.start, willPlay(), origin, std::move(arrived)) != OpenResult::Failed;
    player_.seek(ch.start);
    // The whole track from its start (not a chapter in it): heard again.
    if (book_->remote && (book_->files.size() > 1 || player_.chapters().empty())) startTrackPlay();
    arrived();
    return true;
}

void App::stepChapter(int direction) {
    auto list = chapters();
    int cur = currentChapter();
    int target = cur + direction;
    if (direction < 0) {
        // Like a CD player: first press goes to the start of the current chapter.
        double intoChapter = player_.position() - list[size_t(cur)].start;
        if (intoChapter > 3) target = cur;
    }
    if (target < 0 || target >= int(list.size())) {
        earcons_.play(Earcon::Edge);
        say(target < 0 ? "First " + unit() + "." : "Last " + unit() + ".");
        return;
    }
    if (!gotoChapter(target, Origin::Key)) return; // the error was said
    earcons_.play(Earcon::Move);
    say(list[size_t(target)].title);
}

void App::onFileFinished() {
    scrobble_.reset(); // its play is over: opening it again (Play at the end of the list) is a new one
    if (fileIndex_ + 1 < book_->files.size()) {
        size_t next = fileIndex_ + 1;
        // Quietly: "Loading." between every two tracks of an album would be too much.
        // If a remote one fails, "Server not reachable" is said and it stops there. The
        // finished track was heard to the end, so Play opens the one that failed.
        auto failedHere = [this, next] {
            if (!book_ || !book_->remote) return;
            fileIndex_ = next;
            reopenAt_ = 0.0;
        };
        if (loadFile(next, 0, true, Origin::Next, {}, failedHere) != OpenResult::Failed || book_->remote) return;
    }
    player_.pause();
    if (book_->remote) { // next play starts the list again, opening it only then
        player_.close();
        fileIndex_ = 0;
        reopenAt_ = 0.0;
    } else {
        loadFile(0, 0, false, Origin::Next); // next play starts the book again
    }
    rememberPosition();
    earcons_.play(Earcon::Close);
    say(music_ ? "End of music. Press play to start again." : "End of book. Press play to start again.");
}

// What was buffered has been heard. Moving on to the next track would only wait
// on the same server, so this track stays current and Play opens it again here.
void App::onStreamFailed() {
    double at = player_.position();
    player_.close();
    reopenAt_ = at;
    earcons_.play(Earcon::Error);
    say("Server not reachable. Press play to try again.");
}

// Feedback while a stream that plays has run dry: an earcon, then once
// "Waiting for network", but not over other speech (it waits until that ends).
// Pausing ends it: Play during a dropout gets the feedback again.
void App::checkBuffering(TimePoint now) {
    if (!player_.playing()) {
        dry_.reset();
        return;
    }
    if (!player_.buffering()) {
        if (dry_ && !dry_->playingSince) dry_->playingSince = now;
        if (dry_ && now - *dry_->playingSince >= kPlayingBeforeRecovered) dry_.reset();
        return;
    }
    if (!dry_) dry_ = Dropout{now, {}};
    dry_->playingSince.reset();
    auto dryFor = now - dry_->since;
    if (!dry_->earcon && dryFor >= kDryBeforeEarcon) {
        dry_->earcon = true;
        earcons_.play(Earcon::Buffering);
    }
    if (!dry_->said && dryFor >= kDryBeforeWaitingForNetwork && !tts_.speaking()) {
        dry_->said = true;
        say("Waiting for network.");
    }
}

// The pre-open lasts while the track plays its last kReadAhead and the next
// track is from the same list. Opening that track takes it over; once anything
// else is loaded or opening, or after a pause (which sleep and power off do too),
// or a seek back, it is dropped. A paused stream is not kept waiting: a server
// may close a connection that stays idle.
void App::checkReadAhead() {
    // No length while it is closed or opening, nor for a stream that does not say (a
    // transcode the server lists no length for).
    bool wanted = player_.playing() && book_ && book_->remote && fileIndex_ + 1 < book_->files.size() &&
                  player_.duration() > 0 &&
                  player_.duration() - player_.position() <= std::chrono::duration<double>(kReadAhead).count();
    if (wanted) {
        // A new track, or the quality changed since: pre-open the stream that will be opened.
        size_t next = fileIndex_ + 1;
        const std::string id = book_->files[next].string();
        if (cachedFile(id)) { // a file on disk opens at once: nothing to read ahead, and a stream pre-opened before it was cached is dropped
            if (readAhead_) {
                player_.cancelPreload();
                readAhead_.reset();
            }
            return;
        }
        if (readAhead_ && readAhead_->trackId == id && readAhead_->bitrate == streamingBitrate()) return;
        readAhead_ = ReadAhead{id, streamingBitrate(), newStream(next)};
        player_.preload(readAhead_->stream.url);
    } else if (readAhead_) {
        player_.cancelPreload();
        readAhead_.reset();
    }
}

void App::startTrackPlay() {
    scrobble_ = TrackPlay{};
    scrobble_->trackId = book_->files[fileIndex_].string();
    scrobble_->startedAt = clock_.wallTime();
    // A cached track that plays is kept longer: the time goes to its manifest with the next trimCache().
    // (trimCache() ignores a time before kEarliestPlayTime: a clock that was not set.)
    if (cachedFile(scrobble_->trackId)) played_[scrobble_->trackId] = scrobble_->startedAt;
}

// Heard time is counted with clock_ while the track plays and is not dry, not
// read from the player's position: a seek forward to the middle, or a pause, is
// not listening. Each tick adds the time since the last one if both heard it.
void App::checkScrobble(TimePoint now) {
    if (!scrobble_) return;
    TrackPlay& play = *scrobble_;
    if (!book_ || !player_.playing() || player_.buffering()) {
        play.heardSince.reset();
        return;
    }
    if (!play.nowPlayingSent) {
        play.nowPlayingSent = true;
        server_.scrobble(play.trackId, false);
    }
    if (play.heardSince) play.heard += now - *play.heardSince;
    play.heardSince = now;
    // The player knows the length once open; a transcode has the server's (or none).
    double length = player_.duration() > 0 ? player_.duration()
                    : fileIndex_ < book_->durations.size() ? book_->durations[fileIndex_]
                                                           : 0;
    Clock::TimePoint::duration enough = kScrobbleAtMost;
    if (length > 0)
        enough = std::min(enough, std::chrono::duration_cast<Clock::TimePoint::duration>(
                                      std::chrono::duration<double>(length * kScrobbleShare)));
    if (!play.submitted && play.heard >= enough) {
        play.submitted = true;
        server_.scrobble(play.trackId, true);
    }
}

// The complete downloaded file of remote track `id`, if the cache is set up for the
// server in the config and has one.
std::optional<std::filesystem::path> App::cachedFile(const std::string& id, bool verify) const {
    if (cacheRoot_.empty()) return std::nullopt;
    const TrackCache& store = trackCache();
    if (!store.enabled()) return std::nullopt;
    if (!cacheIndex_) cacheIndex_ = store.index();
    auto it = cacheIndex_->find(id);
    if (it == cacheIndex_->end()) return std::nullopt;
    if (verify) {
        std::error_code ec;
        auto size = std::filesystem::file_size(it->second, ec);
        if (ec || size == 0) { // gone or emptied since the index was made
            cacheIndex_->erase(it);
            return std::nullopt;
        }
    }
    return it->second;
}

namespace {
// For an App given no TextFiles: reads nothing and writes nothing, so its cache is never enabled.
class NoTextFiles : public TextFiles {
public:
    std::optional<std::string> read(const std::filesystem::path&) override { return std::nullopt; }
    bool writeAtomically(const std::filesystem::path&, const std::string&) override { return false; }
};
} // namespace

// The server in the config is set up before the player starts and no menu changes it (a factory
// reset keeps it), so the cache is made once.
const TrackCache& App::trackCache() const {
    static NoTextFiles none;
    if (!trackCache_)
        trackCache_.emplace(textFiles_ ? *textFiles_ : none, textFiles_ ? cacheRoot_ : std::filesystem::path(),
                            config_.getString(kServerUrl), config_.getString(kServerUser));
    return *trackCache_;
}

// ---------------------------------------------------------------- downloads

bool App::canDownload() const {
    return downloader_.configured() && trackCache().enabled(); // no cache folder: not enabled
}

std::string App::downloadProgress() const {
    if (!download_) return "";
    return "Downloading, " + std::to_string(download_->finished) + " of " + countValue(download_->total, "track");
}

namespace {
// The track's file in its list's folder: its id, which is unique, and its type.
std::string downloadFileName(const RemoteTrack& track) {
    std::string name = TrackCache::safeName(track.id);
    if (!track.suffix.empty()) name += "." + TrackCache::safeName(track.suffix);
    return name;
}

// Puts a list's manifest in the order the server has the list now: the tracks it still
// lists come first in its order, then what it no longer lists. Of those, a download the
// user asked for (pinned) is dropped, and its file is deleted by the caller; a track the
// automatic cache holds stays, as a copy of a track that exists that the LRU limit will
// evict. Returns whether the manifest changed, and the dropped entries.
std::pair<bool, std::vector<CachedTrack>> followServerOrder(CachedList& cached, const std::vector<std::string>& order) {
    std::vector<CachedTrack> before = cached.tracks, kept, dropped, extra;
    auto listed = [&](const std::string& id) { return std::find(order.begin(), order.end(), id) != order.end(); };
    for (const std::string& id : order) {
        auto it = std::find_if(before.begin(), before.end(), [&](const CachedTrack& t) { return t.id == id; });
        if (it != before.end()) kept.push_back(*it);
    }
    for (const CachedTrack& t : before)
        if (!listed(t.id)) (t.pinned ? dropped : extra).push_back(t);
    kept.insert(kept.end(), extra.begin(), extra.end());
    bool changed = kept.size() != before.size();
    for (size_t i = 0; !changed && i < kept.size(); ++i) changed = kept[i].id != before[i].id;
    cached.tracks = std::move(kept);
    return {changed, std::move(dropped)};
}
} // namespace

void App::deleteDropped(const RemoteSource& source, const std::vector<CachedTrack>& dropped) {
    const TrackCache& store = trackCache();
    for (const CachedTrack& t : dropped) {
        std::error_code ec;
        std::filesystem::path file = store.listFolder(source) / t.file;
        std::filesystem::remove(file, ec);
        forgetCached(t.id, file);
    }
}

std::string App::trackDestination(const RemoteSource& source, const RemoteTrack& track) const {
    return (trackCache().listFolder(source) / downloadFileName(track)).string();
}

// The index has one file per track id, and a track can be in two lists (an album and a playlist).
void App::forgetCached(const std::string& id, const std::filesystem::path& file) {
    if (!cacheIndex_) return;
    auto it = cacheIndex_->find(id);
    if (it == cacheIndex_->end() || it->second != file) return;
    cacheIndex_->erase(it);
    if (auto other = trackCache().find(id)) (*cacheIndex_)[id] = *other;
}

// A fetched track goes into its list's manifest: the entry is added or replaced, keeping what the
// manifest knows of it (a pinned flag stays; lastPlayed stays unless `playedAt` is a real time, then
// the latest play wins). `pinned`: a download the user asked for. `order`, for a download: the
// list as the server has it, which the manifest then follows (the pinned tracks it drops are
// deleted). `replaceName`: the list is called `title` even if the manifest has another name, else
// a list that has a name keeps it (a list the user downloaded). False if the manifest cannot be written.
bool App::addToManifest(const RemoteSource& source, const RemoteTrack& track, const ManifestUpdate& update) {
    const TrackCache& store = trackCache();
    if (!store.enabled()) return false;
    CachedList cached = store.load(source);
    cached.source = source;
    if (update.replaceName || cached.name.empty()) {
        cached.name = update.title;
        cached.artist = update.artist;
    }
    std::string fileName = downloadFileName(track);
    CachedTrack entry{track.id, fileName, track.title, track.duration, "", update.pinned,
                      update.playedAt >= kEarliestPlayTime ? update.playedAt : 0};
    auto old = std::find_if(cached.tracks.begin(), cached.tracks.end(), [&](const CachedTrack& t) { return t.id == track.id; });
    if (old != cached.tracks.end()) {
        entry.pinned = entry.pinned || old->pinned;
        if (entry.lastPlayed == 0) entry.lastPlayed = old->lastPlayed;
        *old = entry;
    } else {
        cached.tracks.push_back(entry); // appended: a download's manifest then follows the order below
    }
    std::vector<CachedTrack> dropped;
    if (update.order) dropped = followServerOrder(cached, *update.order).second;
    if (!store.save(cached)) return false;
    deleteDropped(source, dropped);
    if (cacheIndex_) (*cacheIndex_)[track.id] = store.listFolder(source) / fileName;
    return true;
}

// OK on a Download item. While that list downloads it says how far it is. A
// download that failed has been dropped, so OK goes on with what is missing:
// tracks already whole are skipped and the .part of the failed one continues.
void App::startDownload(const std::string& title, const std::string& artist, const std::vector<RemoteTrack>& tracks,
                        const RemoteSource& source) {
    if (download_) {
        if (sameList(download_->source, source)) say(downloadProgress() + ".");
        else say("Another download is running.");
        return;
    }
    const TrackCache& store = trackCache();
    if (!store.enabled()) return;
    CachedList cached = store.load(source);
    std::vector<CachedTrack>& manifest = cached.tracks;
    Download job;
    job.source = source;
    job.title = title;
    job.artist = artist;
    bool pinned = false; // a track the automatic cache kept now belongs to the user
    std::uintmax_t needed = 0;
    for (size_t i = 0; i < tracks.size(); ++i) {
        // A track listed twice is one file: fetched and counted once.
        if (std::find(job.order.begin(), job.order.end(), tracks[i].id) != job.order.end()) continue;
        job.order.push_back(tracks[i].id);
        auto entry = std::find_if(manifest.begin(), manifest.end(),
                                  [&](const CachedTrack& t) { return t.id == tracks[i].id; });
        if (entry != manifest.end() && store.completeFile(source, *entry)) {
            job.finished++;
            if (!entry->pinned) entry->pinned = pinned = true;
            continue;
        }
        job.todo.push_back({tracks[i], i + 1});
        if (tracks[i].size > 0) needed += std::uintmax_t(tracks[i].size);
    }
    job.total = job.order.size();
    cached.source = source;
    cached.name = title;
    cached.artist = artist;
    auto [reordered, dropped] = followServerOrder(cached, job.order);
    if ((pinned || reordered || !dropped.empty()) && !store.save(cached)) {
        earcons_.play(Earcon::Error);
        say("Cannot write the download.");
        return;
    }
    deleteDropped(source, dropped);
    if (job.todo.empty()) {
        say("Already downloaded.");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(cacheRoot_, ec);
    std::optional<std::uintmax_t> room = freeSpace_(cacheRoot_);
    if (needed > 0 && room && *room < needed) {
        earcons_.play(Earcon::Error);
        say("Not enough space.");
        return;
    }
    pauseAutoCache(); // an explicit download goes first
    download_ = std::move(job);
    say(downloadProgress() + ".");
    fetchNextTrack();
}

void App::fetchNextTrack() {
    Download& job = *download_;
    const RemoteTrack& track = job.todo[job.next].track;
    job.request = downloader_.download(track.id, trackDestination(job.source, track), track.size, {}, [this](Downloader::Result result) { onTrackDownloaded(result); });
}

void App::onTrackDownloaded(Downloader::Result result) {
    if (!download_) return;
    Download& job = *download_;
    job.request = 0;
    const Download::Item item = job.todo[job.next];
    const RemoteTrack& track = item.track;
    switch (result) {
    case Downloader::Result::Ok: break;
    case Downloader::Result::NotConfigured: failDownload(track, item.number, "No server set up"); return;
    case Downloader::Result::Unreachable: failDownload(track, item.number, "Server not reachable"); return;
    case Downloader::Result::Server: failDownload(track, item.number, "Server error"); return;
    case Downloader::Result::Disk: failDownload(track, item.number, "Cannot write the file"); return;
    }
    if (!addToManifest(job.source, track, {job.title, job.artist, true, true, 0, &job.order})) {
        std::error_code ec; // the whole file is in place but unlisted: nothing would count or evict it
        std::filesystem::remove(trackDestination(job.source, track), ec);
        failDownload(track, item.number, "Cannot write the file");
        return;
    }
    job.finished++;
    if (++job.next < job.todo.size()) fetchNextTrack();
    else finishDownload();
}

// Spoken once; the download is dropped. OK on Download again tries what is missing.
void App::failDownload(const RemoteTrack& track, size_t number, const std::string& reason) {
    if (download_) failedPart_ = trackDestination(download_->source, track) + ".part";
    download_.reset();
    if (sleeping_) return;
    earcons_.play(Earcon::Error);
    std::string name = track.title.empty() ? "track " + std::to_string(number) : track.title;
    say("Cannot download " + name + ". " + reason + ".");
}

// Over something that plays an earcon is enough; otherwise it is said.
void App::finishDownload() {
    std::string title = download_->title;
    download_.reset();
    if (sleeping_) return;
    if (player_.playing()) earcons_.play(Earcon::Open);
    else say("Download finished, " + title + ".");
}

// ---------------------------------------------------------------- automatic cache

std::uintmax_t App::cacheLimit() const {
    return std::uintmax_t(std::max(0, config_.getInt(kCacheLimit, kDefaultCacheGigabytes))) * cacheGigabyte_;
}

// The track that plays has been heard for kAutoCacheAfter: if it streams, it is queued once.
void App::checkAutoCache() {
    if (!scrobble_ || scrobble_->cacheAsked || scrobble_->heard < kAutoCacheAfter) return;
    scrobble_->cacheAsked = true;
    if (!book_ || !book_->remote || !book_->source || fileIndex_ >= book_->files.size() ||
        book_->files[fileIndex_].string() != scrobble_->trackId)
        return;
    std::uintmax_t limit = cacheLimit();
    if (limit == 0) return; // (pumpAutoCache() skips what is cached, and asks about the downloader)
    AutoCache job;
    job.source = *book_->source;
    job.title = book_->title;
    job.artist = book_->artist;
    job.playedAt = scrobble_->startedAt;
    job.track.id = scrobble_->trackId;
    if (fileIndex_ < book_->titles.size()) job.track.title = book_->titles[fileIndex_];
    if (fileIndex_ < book_->durations.size()) job.track.duration = book_->durations[fileIndex_];
    if (fileIndex_ < book_->suffixes.size()) job.track.suffix = book_->suffixes[fileIndex_];
    if (fileIndex_ < book_->sizes.size()) job.track.size = book_->sizes[fileIndex_];
    if (job.track.size > 0 && std::uintmax_t(job.track.size) > limit) return; // would be evicted at once: fetched at every play
    autoQueue_.push_back(std::move(job));
}

void App::pumpAutoCache() {
    if (autoRequest_ || download_ || sleeping_ || autoQueue_.empty()) return;
    if (cacheLimit() == 0 || !canDownload()) { // switched off since, or no server
        autoQueue_.clear();
        trimDirty_ = true; // a .part of the dropped jobs may be left
        return;
    }
    // A download has fetched it meanwhile.
    while (!autoQueue_.empty() && cachedFile(autoQueue_.front().track.id, true)) autoQueue_.pop_front();
    if (autoQueue_.empty()) return;
    // Never fill the card: a track that leaves less than the reserve is skipped, silently, and
    // nothing else starts in this tick (the next one looks at the next track).
    std::error_code ec;
    std::filesystem::create_directories(cacheRoot_, ec);
    std::optional<std::uintmax_t> room = freeSpace_(cacheRoot_);
    std::uintmax_t size = autoQueue_.front().track.size > 0 ? std::uintmax_t(autoQueue_.front().track.size) : 0;
    if (room && *room < size + kAutoCacheReserve) {
        autoQueue_.pop_front();
        return;
    }
    autoJob_ = std::move(autoQueue_.front());
    autoQueue_.pop_front();
    autoRequest_ = downloader_.download(autoJob_->track.id, trackDestination(autoJob_->source, autoJob_->track),
                                        autoJob_->track.size, {}, [this](Downloader::Result r) { onAutoCached(r); });
}

// The transfer in flight stops (its .part stays) and is the next one when the download is over
// or the device wakes.
void App::pauseAutoCache() {
    if (!autoRequest_) return;
    downloader_.cancel(std::exchange(autoRequest_, 0));
    autoQueue_.push_front(std::move(*autoJob_));
    autoJob_.reset();
}

void App::dropAutoCache(const RemoteSource& list) {
    std::erase_if(autoQueue_, [&](const AutoCache& job) { return sameList(job.source, list); });
    if (autoRequest_ && sameList(autoJob_->source, list)) {
        downloader_.cancel(std::exchange(autoRequest_, 0));
        autoJob_.reset();
        trimDirty_ = true; // its .part is abandoned
    }
}

// Silent, whatever happens. A failure leaves its .part, which the next try of that track continues.
void App::onAutoCached(Downloader::Result result) {
    autoRequest_ = 0;
    if (!autoJob_) return;
    AutoCache job = std::move(*autoJob_);
    autoJob_.reset();
    if (result != Downloader::Result::Ok) {
        trimDirty_ = true; // the .part is abandoned unless the track is heard again
        return;
    }
    const TrackCache& store = trackCache();
    if (!store.enabled()) return;
    std::filesystem::path file = store.listFolder(job.source) / downloadFileName(job.track);
    if (!addToManifest(job.source, job.track, {job.title, job.artist, false, false, job.playedAt, nullptr})) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
        return;
    }
    if (cacheBytes_) {
        std::error_code ec;
        auto size = std::filesystem::file_size(file, ec);
        if (!ec) *cacheBytes_ += size;
    }
    trimCache(false);
}

// The .part files that belong to a transfer that runs or waits: a trim must not touch them.
std::vector<std::filesystem::path> App::partsInUse() const {
    std::vector<std::filesystem::path> parts;
    if (!failedPart_.empty()) parts.push_back(failedPart_);
    auto add = [&](const RemoteSource& source, const RemoteTrack& track) {
        parts.push_back(trackDestination(source, track) + ".part");
    };
    if (download_)
        for (size_t i = download_->next; i < download_->todo.size(); ++i) add(download_->source, download_->todo[i].track);
    if (autoJob_) add(autoJob_->source, autoJob_->track);
    for (const AutoCache& job : autoQueue_) add(job.source, job.track);
    return parts;
}

// Evicts what is over the limit; with `flushPlays`, also writes the plays recorded in memory
// into the manifests. Scans the manifests once, and only if there is a reason: the plays to write,
// or the unpinned bytes (counted at the last scan, plus the tracks cached since) are or may be
// over the limit. Called when a track was cached and on exit and sleep, never from a key or a tick.
// A limit of 0 (off) evicts nothing: what is cached stays, only nothing new is added.
void App::trimCache(bool flushPlays) {
    const TrackCache& store = trackCache();
    if (cacheRoot_.empty() || !store.enabled()) return;
    std::uintmax_t limit = cacheLimit();
    bool over = limit > 0 && (trimDirty_ || !cacheBytes_ || *cacheBytes_ > limit);
    if (!over && !(flushPlays && !played_.empty())) return;
    std::vector<std::filesystem::path> keep = partsInUse(); // and the file the player has open
    if (book_ && book_->remote && fileIndex_ < book_->files.size() && player_.isOpen())
        if (auto open = cachedFile(book_->files[fileIndex_].string())) keep.push_back(*open);
    TrackCache::TrimInfo info;
    store.trim(limit > 0 ? limit : std::numeric_limits<std::uintmax_t>::max(), played_, keep, &info);
    played_.clear();
    trimDirty_ = false;
    cacheBytes_ = info.remaining;
    if (cacheIndex_)
        for (const auto& [id, file] : info.evicted) forgetCached(id, file);
}

int App::streamingBitrate() const { return std::max(kOriginalQuality, config_.getInt(kStreamingBitrate, kOriginalQuality)); }

MusicServer::Stream App::remoteStream(size_t file) {
    const std::string id = book_->files[file].string();
    if (readAhead_ && readAhead_->trackId == id && readAhead_->bitrate == streamingBitrate()) return readAhead_->stream;
    return newStream(file);
}

MusicServer::Stream App::newStream(size_t file) {
    MusicServer::Stream stream = server_.stream(book_->files[file].string(), streamingBitrate());
    // A transcode often has no length of its own: the server's listing has it.
    if (stream.transcoded && file < book_->durations.size()) stream.transcoded->duration = book_->durations[file];
    return stream;
}

void App::rememberPosition() {
    if (book_ && book_->remote) { // a stream list: resumed after a restart, if it can be loaded again
        if (!book_->source) return;
        state_.setString(kLastStreamServer, serverIdentity(config_.getString(kServerUrl)));
        state_.setString(kLastStreamUser, config_.getString(kServerUser));
        state_.setString(kLastStreamKind, book_->source->kind == RemoteSource::Kind::Album ? "album" : "playlist");
        state_.setString(kLastStreamId, book_->source->id);
        std::string title = book_->title; // one TSV line: a line break would split it
        std::replace_if(title.begin(), title.end(), [](char c) { return c == '\n' || c == '\r'; }, ' ');
        state_.setString(kLastStreamTitle, title);
        state_.setString(kLastStreamTrack, std::to_string(fileIndex_));
        state_.setString(kLastStreamTrackId, book_->files[fileIndex_].string());
        state_.setString(kLastStreamPos, tenths(place()));
        return;
    }
    if (!book_ || music_ || !player_.isOpen()) return;
    state_.setString("pos:" + book_->path.string(), std::to_string(fileIndex_) + ":" + tenths(player_.position()));
}

double App::place() const {
    return player_.isOpen() ? player_.position() : pendingOpen_ ? pendingOpen_->position : reopenAt_.value_or(0);
}

std::string App::playerStatus() const {
    if (!book_) return "Nothing playing.";
    auto list = chapters();
    int cur = currentChapter();
    double pos = player_.position();
    std::string text = book_->title + ". ";
    if (list.size() > 1)
        text += std::string(music_ ? "Track " : "Chapter ") + std::to_string(cur + 1) + " of " + std::to_string(list.size()) + ", " +
                list[size_t(cur)].title + ". ";
    if (player_.isOpen()) { // else a remote track opened on Play: no place or length yet
        double chapterEnd = player_.duration();
        if (book_->files.size() == 1 && size_t(cur) + 1 < list.size()) chapterEnd = list[size_t(cur) + 1].start;
        text += spokenDuration(chapterEnd - pos) + " left in " + unit() + ". ";
        if (!music_ && book_->files.size() == 1 && list.size() > 1)
            text += spokenDuration(player_.duration() - pos) + " left in book. ";
    }
    text += pendingOpen_ ? "Loading." : player_.playing() ? "Playing." : "Paused.";
    if (sleepDeadline_)
        text += " Sleep timer, " + spokenDuration(std::chrono::duration<double>(*sleepDeadline_ - clock_.now()).count()) + " left.";
    return text;
}

App::NowPlaying App::nowPlaying() const {
    NowPlaying np;
    if (!book_) return np;
    auto list = chapters();
    int cur = currentChapter();
    np.loaded = true;
    np.playing = player_.playing();
    np.title = book_->title;
    np.chapter = list.empty() ? "" : list[size_t(cur)].title;
    np.chapterIndex = cur;
    np.chapterCount = int(list.size());
    np.position = place();
    np.duration = player_.duration();
    if (sleepDeadline_) np.sleepTimerLeft = std::chrono::duration<double>(*sleepDeadline_ - clock_.now()).count();
    return np;
}

// ---------------------------------------------------------------- power

void App::setSleepTimer(int minutes, bool endOfChapter) {
    sleepDeadline_.reset();
    sleepAtChapterEnd_ = -1;
    if (endOfChapter) {
        sleepAtChapterEnd_ = currentChapter();
        say("Sleep timer set to end of " + unit() + ".");
    } else if (minutes > 0) {
        sleepMinutes_ = minutes;
        sleepDeadline_ = clock_.now() + std::chrono::minutes(minutes);
        say("Sleep timer set to " + spokenDuration(minutes * 60) + ".");
    } else {
        say("Sleep timer off.");
    }
}

void App::sleep(const std::string& announcement) {
    cancelResume();
    dropPendingOpen(); // nothing may start playing while the device sleeps
    pause(false);
    pauseAutoCache(); // no transfer while asleep: it goes on from its .part after the wake
    saveOnExit(); // sleep on the device may end in the power being cut
    sleepDeadline_.reset();
    sleepAtChapterEnd_ = -1;
    sleeping_ = true;
    earcons_.play(Earcon::Sleep);
    say(announcement);
}

void App::wake() {
    tts_.stop();
    sleeping_ = false;
    earcons_.play(Earcon::Wake);
    if (mode() == Mode::Player && book_) {
        auto list = chapters();
        say(book_->title + ", paused. " + list[size_t(currentChapter())].title + ".");
    } else if (!screens_.empty()) {
        say(screens_.back().title + ". " + itemSpeech(screens_.back()));
    } else {
        goHome();
    }
}

void App::powerOff() {
    cancelResume(); // the front end ticks on while "Goodbye" is spoken
    dropPendingOpen(); // nothing may start playing while the device shuts down
    pause(false);
    saveOnExit();
    earcons_.play(Earcon::Sleep);
    say("Powering off. Goodbye.");
    quit_ = true;
}

void App::restart(bool saveFirst) {
    if (saveFirst) saveOnExit();
    pendingOpen_.reset();
    player_.close();
    book_.reset();
    music_ = false;
    closeScreens(0);
    sleepDeadline_.reset();
    sleepAtChapterEnd_ = -1;
    keyDescriber_ = false;
    say("Restarting.");
    start();
}

// Clears what the user set and the state, but keeps the server: a blind user
// must not lose a server someone else set up for them.
void App::factoryReset() {
    config_.clearExcept(kServerPrefix);
    state_.clear();
    saveConfig();
    saveState();
    say("Settings erased.");
    restart(false); // saving first would put the position straight back
}

std::string App::statusReport() const {
    std::string text = "Battery 80 percent, simulated. ";
    std::error_code ec;
    auto space = std::filesystem::space(libraryRoot_, ec);
    if (!ec) text += std::to_string(space.available / (1024 * 1024 * 1024)) + " gigabytes free. ";
    text += "Wi-Fi connected. ";
    char buf[32];
    std::time_t t = clock_.wallTime();
    std::strftime(buf, sizeof buf, "Time %H:%M.", std::localtime(&t));
    return text + buf;
}

} // namespace oap
