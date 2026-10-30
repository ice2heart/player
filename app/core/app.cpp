#include "app.h"

#include "album_merge.h"

#include "queue_file.h"
#include "text.h"
#include "track_cache.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <set>
#include <limits>
#include <memory>
#include <random>
#include <system_error>
#include <utility>

namespace oap {

namespace {
constexpr const char* kVersion = "0.1";
constexpr size_t kSpeechLogSize = 100;
constexpr std::chrono::seconds kProgressInterval{60};
constexpr double kProgressMovement = 5;
constexpr std::int64_t kProgressMillisPerSecond = 1000;

// Setting keys
constexpr const char* kSpeechRate = "speech.rate";
constexpr const char* kVolume = "volume";
constexpr int kMenuJump = 10; // items moved by a deep Back / Forward press
constexpr const char* kUnknownArtist = "Unknown artist";
constexpr int kVolumeStep = 5; // percent per press of a volume key
constexpr const char* kVerbosity = "verbosity"; // 0 beginner, 1 normal, 2 expert
constexpr const char* kRewindOnResume = "rewind_on_resume";
constexpr const char* kLongPress = "buttons.long_press_ms";
constexpr const char* kMenuWrap = "menu.wrap";
constexpr const char* kAutoSleep = "power.auto_sleep_min";
constexpr const char* kStreamingBitrate = "streaming.bitrate"; // kbit/s of a transcode; 0: the original file
constexpr const char* kDownloadBitrate = "downloads.bitrate";
constexpr const char* kCacheUpgrade = "cache.upgrade"; // prefer a better stream to a complete cached copy
constexpr const char* kCacheLimit = "cache.limit_gb"; // automatic cache: gigabytes of unpinned tracks, 0: off
constexpr const char* kLastBook = "last_book";
// The player used last, "music" or "book": which one Home lists first and start() resumes.
constexpr const char* kLastPlayer = "last_player";
constexpr const char* kPlayerMusic = "music";
constexpr const char* kPlayerBook = "book";
constexpr double kSeekStep = 5; // seconds per normal seek repeat
constexpr double kDeepSeekStep = 30;
constexpr std::chrono::milliseconds kSeekInterval{500};
// The catalogue version of the server the index was last synced with (getArtists `lastModified`),
// and the server key it belongs to. In state.tsv, never in config.yaml.
constexpr const char* kOldCatalogueServer = "catalogue.server"; // before several servers: removed by the first sync
constexpr const char* kOldCatalogueVersion = "catalogue.version";
// The audiobook folder the server items were sorted with (kCatalogueVersion alone would not
// change when the setting does), and the setting itself: config.yaml's, never written by the device.
constexpr const char* kOldCatalogueFolder = "catalogue.folder";
constexpr size_t kRecentAlbums = 50; // the most albums Recently added and Recently played list
constexpr int kCataloguePage = 500;          // albums per request (the most getAlbumList2 gives)
constexpr size_t kCatalogueLimit = 200000;   // a server that never stops paging is an error
// The remote list that played last, resumed by "Now playing" after a restart.
// The server is named by its url and user (never the password, nor a stream
// URL, which holds the login), so a place on another server is not resumed.
constexpr const char* kLastStreamPrefix = "last_stream."; // all the keys below
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
// A chapter that starts this close before a file's start, on the book's timeline, starts that file: durations
// the server adds up in floating point can fall a hair short of the chapter start it wrote for the same instant.
constexpr double kChapterSlack = 0.05;
// Chapters this close to a chapter start count as inside it: the position of a player is not exact.
constexpr double kChapterPositionSlack = 0.25;
// Previous chapter within this many seconds of a chapter's start goes to the chapter before it.
constexpr double kPreviousRestartAfter = 3;
// Ignore small differences between independently saved listening places.
constexpr double kServerPlaceDistance = 30;
// A server that is marked down is pinged in the background (docs/queue.md, "Playing it"): this long after
// it was marked, then this long after the first ping went unanswered, then every kPingEvery.
constexpr std::chrono::seconds kPingFirst{10};
constexpr std::chrono::seconds kPingSecond{30};
constexpr std::chrono::seconds kPingEvery{60};
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
    "This player streams from Subsonic servers, such as Navidrome, and from Audiobookshelf, and only when it is "
    "built with network support. Check the type in the servers section of config.yaml.";

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
    {Button::Back, "Left", "In menus, previous item. Press deeper to go back ten items; hold for the first item. In a player, previous chapter or track, or restart after three seconds. Hold to seek back; press deeper to seek faster. Release to stop seeking."},
    {Button::Forward, "Right", "In menus, next item. Press deeper to go forward ten items; hold for the last item. In a player, next chapter or track. Hold to seek forward; press deeper to seek faster. Release to stop seeking."},
    {Button::Up, "Up", "Unused during normal use."},
    {Button::Down, "Down", "Unused during normal use."},
    {Button::Ok, "OK", "Opens the selected item. In a player, opens the player menu. Hold for options about the selected item or current playback."},
    {Button::Home, "Esc", "Goes back to the previous screen. At Home, returns to the current player without resuming it. Hold for the Home menu."},
    {Button::VolumeUp, "Volume up", "Raises the volume, in menus and in the player. Hold to keep raising it."},
    {Button::VolumeDown, "Volume down", "Lowers the volume, in menus and in the player. Hold to keep lowering it."},
    {Button::Options, "Options", "Actions for the selected item. Says when there are none."},
    {Button::Talk, "Talk", "Unused during normal use."},
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

Screen playerScreen(bool music) {
    return {music ? Screen::Kind::MusicPlayer : Screen::Kind::BookPlayer, "Now playing", {}, {}};
}

// The default random numbers of Shuffle: one generator for the run, seeded from the clock (core reads no
// other source of chance; a front end or a test can set its own with setRandom()).
QueueRandom clockRandom(const Clock& clock) {
    const std::uint64_t seed = static_cast<std::uint64_t>(clock.now().time_since_epoch().count()) ^
                               (static_cast<std::uint64_t>(clock.wallTime()) << 20);
    auto generator = std::make_shared<std::mt19937_64>(seed);
    return [generator](std::size_t bound) { return std::uniform_int_distribution<std::size_t>(0, bound - 1)(*generator); };
}

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

App::App(Services services, std::filesystem::path libraryRoot, std::filesystem::path musicRoot,
         std::filesystem::path cacheRoot, std::filesystem::path queueFile, std::filesystem::path scrobbleFile)
    : tts_(services.speech), player_(services.player), earcons_(services.sounds), volume_(services.volume),
      servers_(std::move(services.servers)), index_(services.libraryStore), scanner_(services.scanner),
      config_(services.config), state_(services.state), savedState_(services.state.values()),
      queueFile_(std::move(queueFile)), clock_(services.clock), halEvents_(services.events), power_(services.power), storage_(services.storage), libraryRoot_(std::move(libraryRoot)),
      musicRoot_(std::move(musicRoot)), cacheRoot_(std::move(cacheRoot)), freeSpace_(spaceOnDisk),
      random_(clockRandom(services.clock)), lastActivity_(services.clock.now()) {
    textFiles_ = services.files;
    localPlaylists_ = services.localPlaylists;
    scrobbleFile_ = std::move(scrobbleFile);
    links_.resize(std::max<size_t>(1, servers_.size()));
}

// ---------------------------------------------------------------- lifecycle

void App::start() {
    lastPlaylist_ = state_.getString("last_playlist");
    tts_.setRate(config_.getInt(kSpeechRate, 220));
    volume_.setVolume(config_.getInt(kVolume, 70) / 100.0f);
    gestures_.setLongPress(std::chrono::milliseconds(config_.getInt(kLongPress, 600)));
    lastActivity_ = clock_.now();

    earcons_.play(Earcon::Boot);
    say("Ready.");
    startScan(); // the menus use the saved index until it is done
    startSync(); // silent: the saved server items stay if it fails
    loadScrobbles(); // what was heard and not taken by its server (a cut power, an exit) is sent now
    sendScrobbles();
    retryProgress();

    // The music queue of queue.tsv is in the music player now, closed. Whichever player was used last has
    // "Now playing" on Home; a book that was last is opened below, which parks the music queue.
    restoreQueue();

    // The music player used last: "Now playing" on Home plays its queue (or, from an older version, loads
    // the stream saved as `last_stream`). Nothing is asked of the server now, and nothing plays until the
    // user chooses it.
    // A server book played last (`last_book` is its item id, `<server key>:album:<id>`, which no local
    // book is, as a local path is absolute) is resumed from Home the same way, and is not looked for on disk.
    // Whichever player was used first is the one listed as "Now playing"; the other is "Continue".
    std::string last = state_.getString(kLastBook);
    if ((lastPlayerIsMusic() && ((book_ && music_) || savedStream())) || isServerBookId(last)) {
        goHome();
        return;
    }
    if (auto b = resumableBook(last); b && openBook(*b, false)) {
        closeScreens(0);
        pushScreen(menuScreen("Home", [this] { return homeMenu(); }));
        pushScreen(playerScreen(false));
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

namespace {
std::string keyOfRef(const std::string& ref) { return ref.substr(0, ref.find(':')); }

// "catalogue.<server key>.version": what the server's catalogue was when the index was last synced with it
// (getArtists `lastModified`), and "catalogue.<server key>.folder": the audiobook folder its items were sorted with.
std::string versionKey(const std::string& serverKey) { return "catalogue." + serverKey + ".version"; }
std::string folderKey(const std::string& serverKey) { return "catalogue." + serverKey + ".folder"; }
} // namespace

std::optional<App::ServerRef> App::serverRefOf(const std::vector<std::string>& seenOn) const {
    for (size_t link = 0; link < servers_.size(); ++link) {
        if (servers_[link].key.empty()) continue;
        if (auto ref = LibraryIndex::refOnServer(seenOn, servers_[link].key)) return ServerRef{link, *ref};
    }
    return std::nullopt;
}

// What was seen on a server that is not set up now (removed from config.yaml, or the same one under another
// user), or on any server once none is set up, is gone: its server items, its references, its playlists and
// the version it was synced at. Items on this device stay. The keys of the single-server catalogue go too:
// the first sync of every server asks again.
void App::dropUnsetServers() {
    std::set<std::string> others;
    for (const auto& [id, item] : index_.items()) {
        for (const auto& ref : item.seenOn) others.insert(keyOfRef(ref));
        for (const auto& track : item.tracks)
            for (const auto& ref : track.seenOn) others.insert(keyOfRef(ref));
    }
    for (const auto& server : index_.playlistServers()) others.insert(server);
    for (const ServerLink& link : servers_) others.erase(link.key);
    for (const auto& other : others) {
        index_.replaceServer(other, {}, clock_.wallTime());
        index_.replacePlaylists(other, {});
    }
    index_.save();
    // The versions of servers that are not set up, whether or not the index holds anything of them.
    std::set<std::string> keep;
    for (const ServerLink& link : servers_) {
        keep.insert(versionKey(link.key));
        keep.insert(folderKey(link.key));
    }
    const std::string prefix = "catalogue.";
    std::vector<std::string> stale;
    for (const auto& [name, value] : state_.values())
        if (name.starts_with(prefix) && !keep.contains(name))
            for (const char* suffix : {".version", ".folder"})
                if (name.size() > prefix.size() + std::strlen(suffix) && name.ends_with(suffix)) stale.push_back(name);
    for (const std::string& name : stale) state_.erase(name);
    state_.erase(kOldCatalogueServer);
    state_.erase(kOldCatalogueVersion);
    state_.erase(kOldCatalogueFolder);
}

namespace {
std::string downloadedName(const CachedList& list); // below, with the downloaded lists
bool isWholeDownload(const CachedList& list, const std::vector<RemoteTrack>& onDisk);
}

// The pinned albums of the caches are the Cached items of the index. A download carries the
// reference of the server it came from, taken from its manifest: the album's own and one per
// track. The reference is kept only while the server still lists the album (or while no
// catalogue of this server is known yet), because an album that left the server has no
// reference. Playlists are not albums: the index keeps them apart (fetchPlaylists()).
void App::syncDownloads() {
    std::vector<IndexItem> items;
    for (size_t link = 0; link < servers_.size(); ++link) {
        const TrackCache& store = trackCacheOf(link);
        if (!store.enabled()) continue;
        const std::string& key = servers_[link].key;
        // Everything a Books server has downloaded is a book: listed in its Downloaded, never in Music.
        if (servers_[link].server.catalogue() == MusicServer::Catalogue::Books) continue;
        bool catalogue = false;
        for (const auto& [id, item] : index_.items())
            if (item.source == IndexItem::Source::Server && LibraryIndex::refOnServer(item.seenOn, key)) catalogue = true;
        for (const CachedList& list : store.lists(true)) {
            if (list.source.kind != RemoteSource::Kind::Album) continue;
            std::string ref = LibraryIndex::seenRef(key, SeenKind::Album, list.source.id);
            const IndexItem* onServer = index_.find(ref);
            // A downloaded audiobook of the server stays a book (Library, not Music): it plays its files
            // through the server book (openServerBook()).
            if (onServer && onServer->source == IndexItem::Source::Server && onServer->kind == IndexItem::Kind::Book) continue;
            IndexItem item;
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
            if (!catalogue || (onServer && onServer->source == IndexItem::Source::Server)) item.seenOn.push_back(ref);
            items.push_back(std::move(item));
        }
    }
    index_.replaceSource(IndexItem::Source::Cached, items, clock_.wallTime());
}

// Local and downloaded items with no reference to a server that are one of its albums by the
// name rule get its reference (merged entries keep it, so a later sync matches by it).
void App::linkToServer() {
    for (const ServerLink& link : servers_) {
        if (link.key.empty()) continue;
        for (const AlbumLink& found : linkByName(link.key, index_.items())) index_.addSeenOn(found.itemId, found.reference);
    }
}

// Every server in turn, in config order: one request at a time, so the device does one thing over
// Wi-Fi. A sync that is already running (the start-up one) is not started twice.
void App::startSync() {
    dropUnsetServers(); // with no server set up that is every server
    if (syncRunning_) return;
    syncRunning_ = true;
    syncLink_ = 0;
    syncTried_ = 0;
    syncFailures_.clear();
    syncNext();
}

void App::syncNext() {
    while (syncLink_ < servers_.size() && (!servers_[syncLink_].server.configured() || servers_[syncLink_].key.empty())) ++syncLink_;
    if (syncLink_ >= servers_.size()) { // nothing left to ask
        syncRunning_ = false;
        return finishRefresh();
    }
    ++syncTried_;
    catalogue_ = {};
    catalogue_.link = syncLink_;
    catalogue_.key = servers_[syncLink_].key;
    catalogueRequest_ = servers_[syncLink_].server.catalogueVersion([this](MusicServer::Error error, std::string version) {
        catalogueRequest_ = 0;
        onCatalogueVersion(error, version);
    });
}

void App::onCatalogueVersion(MusicServer::Error error, const std::string& version) {
    if (error != MusicServer::Error::None) return finishSync(error);
    // A Books server (Audiobookshelf) has no cheap check: its whole book list is fetched every time and
    // only what changed is written. Everything it lists is a book; it has no folder and no version to keep.
    if (servers_[catalogue_.link].server.catalogue() == MusicServer::Catalogue::Books) {
        catalogue_.books = true;
        return fetchCatalogue(0);
    }
    bool haveItems = false;
    for (const auto& [id, item] : index_.items())
        if (item.source == IndexItem::Source::Server && LibraryIndex::refOnServer(item.seenOn, catalogue_.key))
            haveItems = true;
    const std::string folder = servers_[catalogue_.link].config.audiobookFolder;
    // A server that gives no version cannot be told unchanged. Without server items in the
    // index (it was lost) the version does not matter either.
    if (!version.empty() && haveItems && state_.getString(versionKey(catalogue_.key)) == version &&
        state_.getString(folderKey(catalogue_.key)) == folder)
        return fetchPlaylists(); // the catalogue is as it was, the playlists may not be
    catalogue_.version = version;
    catalogue_.folder = folder;
    if (catalogue_.folder.empty()) fetchCatalogue(0);
    else fetchBookFolder();
}

// The server's audiobook_folder names a music folder: its albums are the audiobooks. The ids of its
// albums are fetched first, then the whole catalogue, and the ones in the folder become books.
// A folder the server does not have makes no books, and the sync is not remembered (the name may be
// a typo, and the next start asks again).
void App::fetchBookFolder() {
    catalogueRequest_ = servers_[catalogue_.link].server.musicFolders([this](MusicServer::Error error, std::vector<RemoteFolder> folders, bool) {
        catalogueRequest_ = 0;
        if (error != MusicServer::Error::None) return finishSync(error);
        for (const RemoteFolder& f : folders)
            if (f.name == catalogue_.folder) return fetchBookIds(f.id, 0);
        catalogue_.version.clear(); // applyCatalogue() then stores neither the version nor the folder
        fetchCatalogue(0);
    });
}

void App::fetchBookIds(const std::string& folderId, int offset) {
    catalogueRequest_ = servers_[catalogue_.link].server.albums(
        "alphabeticalByName", offset, kCataloguePage,
        [this, folderId, offset](MusicServer::Error error, std::vector<RemoteAlbum> albums, bool more) {
            catalogueRequest_ = 0;
            if (error != MusicServer::Error::None) return finishSync(error);
            for (const RemoteAlbum& a : albums) catalogue_.bookIds.insert(a.id);
            if (more && !albums.empty() && catalogue_.bookIds.size() < kCatalogueLimit)
                return fetchBookIds(folderId, offset + int(albums.size()));
            if (more && !albums.empty()) return finishSync(MusicServer::Error::Server);
            fetchCatalogue(0);
        },
        folderId);
}

void App::fetchCatalogue(int offset) {
    catalogueRequest_ = servers_[catalogue_.link].server.albums(
        "alphabeticalByName", offset, kCataloguePage,
        [this, offset](MusicServer::Error error, std::vector<RemoteAlbum> albums, bool more) {
            catalogueRequest_ = 0;
            if (error != MusicServer::Error::None) return finishSync(error);
            for (const RemoteAlbum& a : albums) {
                IndexItem item;
                item.id = LibraryIndex::seenRef(catalogue_.key, SeenKind::Album, a.id);
                item.source = IndexItem::Source::Server;
                item.kind = catalogue_.books || catalogue_.bookIds.count(a.id) ? IndexItem::Kind::Book : IndexItem::Kind::Music;
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
            fetchPlaylists();
        });
}

// The last step of a sync: the server's playlists replace the stored ones. If the request fails the old
// ones stay (the sync ends with the error, as for the catalogue).
void App::fetchPlaylists() {
    catalogueRequest_ = servers_[catalogue_.link].server.playlists([this](MusicServer::Error error, std::vector<RemotePlaylist> playlists, bool) {
        catalogueRequest_ = 0;
        if (error != MusicServer::Error::None) return finishSync(error);
        std::vector<IndexPlaylist> stored;
        for (const RemotePlaylist& p : playlists) {
            IndexPlaylist playlist;
            playlist.server = catalogue_.key;
            playlist.id = p.id;
            playlist.name = p.name;
            playlist.songCount = p.songCount;
            playlist.changed = parseIsoTime(p.changed);
            stored.push_back(std::move(playlist));
        }
        // A playlist saved while this request was out may be missing from the answer.
        for (const IndexPlaylist& added : catalogue_.savedPlaylists)
            if (std::none_of(stored.begin(), stored.end(), [&](const IndexPlaylist& p) { return p.id == added.id; })) stored.push_back(added);
        for (auto& playlist : stored) {
            auto edited = catalogue_.editedPlaylists.find(playlist.id);
            if (edited != catalogue_.editedPlaylists.end()) playlist = edited->second;
        }
        for (const auto& [id, edited] : catalogue_.editedPlaylists)
            if (std::none_of(stored.begin(), stored.end(), [&](const IndexPlaylist& playlist) { return playlist.id == id; }))
                stored.push_back(edited);
        stored.erase(std::remove_if(stored.begin(), stored.end(), [this](const IndexPlaylist& playlist) {
            return catalogue_.deletedPlaylists.contains(playlist.id);
        }), stored.end());
        for (const auto& [ref, old] : index_.playlists()) {
            if (old.server != catalogue_.key || !state_.getInt(keptPlaylistKey(catalogue_.link, old.id), 0)) continue;
            if (std::none_of(stored.begin(), stored.end(), [&](const IndexPlaylist& p) { return p.id == old.id; }))
                setPlaylistKept(catalogue_.link, old.id, old.name, false);
        }
        for (const auto& old : trackCacheOf(catalogue_.link).lists()) {
            if (old.source.kind != RemoteSource::Kind::KeptPlaylist || !state_.getInt(keptPlaylistKey(catalogue_.link, old.source.id), 0)) continue;
            if (std::none_of(stored.begin(), stored.end(), [&](const IndexPlaylist& p) { return p.id == old.source.id; }))
                setPlaylistKept(catalogue_.link, old.source.id, old.name, false);
        }
        for (const auto& p : stored)
            if (state_.getInt(keptPlaylistKey(catalogue_.link, p.id), 0))
                scheduleKeptPlaylist(catalogue_.link, p.id, p.name);
        index_.replacePlaylists(catalogue_.key, stored);
        index_.savePlaylists();
        finishSync(MusicServer::Error::None);
    });
}

// Every page came: the server items of the index become the albums fetched.
void App::applyCatalogue() {
    index_.replaceServer(catalogue_.key, catalogue_.items, clock_.wallTime());
    catalogue_.items.clear();
    catalogue_.bookIds.clear();
    syncDownloads(); // a download of an album the server dropped loses its reference
    linkToServer();
    // Only the items decide: a playlists table that cannot be written must not make the next start fetch the albums again.
    if (index_.saveItems() && !catalogue_.version.empty()) { // else the next start checks again
        state_.setString(versionKey(catalogue_.key), catalogue_.version);
        if (catalogue_.folder.empty()) state_.erase(folderKey(catalogue_.key)); // as before this setting
        else state_.setString(folderKey(catalogue_.key), catalogue_.folder);
    }
}

// The sync of this server is over, and the next one starts. An error before the albums came leaves the
// server's items as they were; one from getPlaylists leaves the albums replaced and the playlists as they were.
// Another server's error does not stop the others.
void App::finishSync(MusicServer::Error error) {
    catalogue_.items.clear();
    catalogue_.bookIds.clear();
    catalogue_.savedPlaylists.clear();
    if (error != MusicServer::Error::None) syncFailures_.push_back({catalogue_.link, error});
    syncLink_ = catalogue_.link + 1;
    if (error == MusicServer::Error::None) sendScrobbles(); // the server answers
    syncNext();
}

// Music -> Refresh library: rescans the files and, with servers, checks their catalogues. A scan or
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
    if (!syncRunning_) startSync();
}

// Called when a scan or a sync ends: when none is left, a Refresh that waits speaks its result,
// but only on the screen it started on and while awake (a late result is never spoken).
void App::finishRefresh() {
    if (!refresh_.active || scanRequest_ || syncRunning_) return;
    Refresh done = std::exchange(refresh_, {});
    if (sleeping_ || screens_.empty() || screens_.back().id != done.screen) return;
    std::string text;
    if (syncFailures_.size() < syncTried_ || syncTried_ == 0) { // some server answered, or none was asked
        syncDownloads(); // a merged album counts once
        index_.save();
        const size_t albums = mergeAlbums(index_.items()).size();
        size_t books = 0; // the books of the servers; local books are not counted
        for (const auto& [id, item] : index_.items())
            if (item.source == IndexItem::Source::Server && item.kind == IndexItem::Kind::Book) ++books;
        text = "Library updated" + (albums ? ", " + countValue(albums, "album") : std::string()) +
               (books ? ", " + countValue(books, "book") : std::string()) + ".";
    }
    // A server that fails says its name (with one server there is nothing to tell apart) and the error.
    for (const SyncFailure& failure : syncFailures_) {
        std::string error = errorText(failure.error);
        if (servers_.size() > 1) {
            error[0] = char(std::tolower(static_cast<unsigned char>(error[0])));
            error = linkName(failure.link) + ": " + error;
        }
        text += (text.empty() ? "" : " ") + error + ".";
    }
    say(text);
}

void App::shutdown() {
    cancelStarOptions();
    cancelPlaylistEdit();
    if (shutDown_) return; // the player is closed now: a second save would write its place as 0
    cancelGestures();
    cancelProgressPull();
    cancelLoading();
    cancelSaving();
    for (auto& [id, list] : remoteLists_)
        if (list.request) serverAt(list.link).cancel(list.request);
    remoteLists_.clear();
    shutDown_ = true;
    saveOnExit();
    for (auto [link, request] : progressRetries_) serverAt(link).cancel(request);
    progressRetries_.clear();
    cancelResume();
    if (download_) servers_[download_->link].downloader.cancel(download_->request); // the .part stays: OK on Download goes on from it
    download_.reset();
    if (autoRequest_) cancelAutoRequest();
    autoJob_.reset();
    autoQueue_.clear();
    if (scanRequest_) scanner_.cancel(std::exchange(scanRequest_, 0));
    if (catalogueRequest_) serverAt(catalogue_.link).cancel(std::exchange(catalogueRequest_, 0));
    if (keptRequest_) serverAt(keptRequestLink_).cancel(std::exchange(keptRequest_, 0));
    keptJobs_.clear();
    syncRunning_ = false;
    syncFailures_.clear();
    syncTried_ = 0;
    catalogue_ = {};
    refresh_ = {};
    pendingOpen_.reset();
    player_.cancelPreload();
    player_.close();
}

void App::keyDown(Button button) {
    if (shutDown_ || held_[static_cast<size_t>(button)]) return;
    held_[static_cast<size_t>(button)] = true;
    locked_[static_cast<size_t>(button)] = false; // a fresh contact cycle clears its prior lock
    if (hold_ && button != Button::Power) {
        tts_.stop();
        earcons_.play(Earcon::Error);
        say("Locked.");
        return;
    }
    if (auto ev = gestures_.press(button, clock_.now())) onGesture(*ev);
}

void App::keyUp(Button button) {
    if (shutDown_) return;
    held_[static_cast<size_t>(button)] = false;
    auto ev = gestures_.release(button, clock_.now());
    if (std::exchange(locked_[static_cast<size_t>(button)], false)) return; // held down when Hold was switched on
    if (ev) onGesture(*ev);
}

void App::onGesture(ButtonEvent ev) {
    if (ev.gesture == Gesture::Cancel || ev.gesture == Gesture::Release) {
        if (seeking_ && seeking_->button == ev.button) finishSeeking(ev.gesture == Gesture::Release);
        return;
    }
    if (locked_[static_cast<size_t>(ev.button)] ||
        (ev.button == Button::Back && locked_[static_cast<size_t>(Button::BackDeep)]) ||
        (ev.button == Button::Forward && locked_[static_cast<size_t>(Button::ForwardDeep)])) return;
    if (ev.gesture == Gesture::Light) {
        if (seeking_ && seeking_->button == ev.button) seeking_->deep = false;
        return;
    }
    onButton(ev);
}

void App::cancelGestures(bool preservePower) {
    for (const ButtonEvent& ev : gestures_.cancel(preservePower)) onGesture(ev);
    finishSeeking(false);
    for (size_t i = 0; i < kButtonCount; ++i)
        if (held_[i] && !(preservePower && static_cast<Button>(i) == Button::Power)) locked_[i] = true;
}

// File changes affect navigation holds, but volume and held Play remain global.
void App::cancelNavigation() {
    for (const ButtonEvent& ev : gestures_.cancelNavigation()) onGesture(ev);
    finishSeeking(false);
    for (Button button : {Button::Back, Button::Forward, Button::BackDeep, Button::ForwardDeep})
        if (held_[static_cast<size_t>(button)]) locked_[static_cast<size_t>(button)] = true;
}

void App::setHold(bool on) {
    if (shutDown_) return;
    if (hold_ == on) return;
    hold_ = on;
    // A key already down when Hold is switched on does nothing more: no long
    // press while it is held, no short press when it is let go. Power still works.
    if (on) cancelGestures(true);
    tts_.stop();
    earcons_.play(on ? Earcon::Close : Earcon::Open);
    say(on ? "Buttons locked." : "Buttons unlocked.");
}

App::Mode App::mode() const {
    if (sleeping_) return Mode::Sleeping;
    return !screens_.empty() && screens_.back().isPlayer() ? Mode::Player : Mode::Menu;
}

void App::batteryChanged(const BatteryState& state) {
    const bool startedCharging = state.charging && (!lastBattery_ || !lastBattery_->charging);
    const bool becameFull = state.full && (!lastBattery_ || !lastBattery_->full);
    lastBattery_ = state;
    if (startedCharging) {
        battery20Announced_ = battery10Announced_ = battery5Announced_ = false;
        batteryCriticalHandled_ = false;
        say("Charging started.");
    }
    if (becameFull) say("Battery full.");
    if (state.charging) return;
    if (state.percent <= 20 && !battery20Announced_) {
        battery20Announced_ = true;
        say("Battery 20 percent.");
    }
    if (state.percent <= 10 && !battery10Announced_) {
        battery10Announced_ = true;
        say("Battery 10 percent.");
    }
    if (state.percent <= 5 && !battery5Announced_) {
        battery5Announced_ = true;
        tts_.stop();
        say("Battery 5 percent.");
    }
    if (state.percent <= 3 && !batteryCriticalHandled_) {
        batteryCriticalHandled_ = true;
        if (power_ && power_->caps().powerOff) {
            saveOnExit();
            power_->requestPowerOff();
        } else {
            tts_.stop();
            say("Battery critical.");
        }
    }
}

void App::tick() {
    if (halEvents_) {
        for (const auto& event : halEvents_->drain()) {
            if (const auto* error = std::get_if<HalError>(&event); error && !shutDown_) {
                halErrors_.push_back(error->message);
                say(error->message);
            } else if (const auto* battery = std::get_if<BatteryChanged>(&event); battery && !shutDown_) {
                batteryChanged(battery->state);
            }
        }
    }
    auto now = clock_.now();
    if (!shutDown_)
        for (const ButtonEvent& ev : gestures_.poll(now)) onGesture(ev);
    if (servers_.empty()) { // the stand-ins answer through poll() too
        noServer_.poll();
        noDownloader_.poll();
    }
    for (ServerLink& link : servers_) {
        link.server.poll(); // answers from the server fill their lists
        link.downloader.poll(); // a finished track starts the next one
    }
    scanner_.poll();    // a finished scan updates the index
    if (shutDown_) return; // exit acknowledgements above still clear saved outboxes
    if (pendingOpen_ && !player_.opening()) finishPendingOpen();
    else if (pendingOpen_ && now - pendingOpen_->since > kStreamOpenGiveUp) giveUpPendingOpen();

    // The end of a file is the end of a chapter, even when the next chapter has the same number (a one-file
    // book or a loose track starts again), but for a server chapter that goes on in the next file.
    if (book_ && player_.failed()) { cancelNavigation(); onStreamFailed(); }
    repeatSeeking(now);
    bool fileFinished = book_ && !seeking_ && !seekAtEnd_ && player_.finished();
    const bool chapterFinished = fileFinished && chapterEndsWithFile(); // asked before the next file opens
    if (fileFinished) onFileFinished();
    checkBuffering(now);
    checkScrobble(now);
    checkProgress(now);
    checkAutoCache();
    pumpKeptPlaylists();
    pumpAutoCache();

    if (sleepDeadline_ && now >= *sleepDeadline_) {
        sleepDeadline_.reset();
        sleep("Sleep timer ended.");
    }
    // While a track opens, the current one is the track it would replace if it fails.
    if (sleepAtChapterEnd_ >= 0 && book_ &&
        (chapterFinished || (!pendingOpen_ && currentChapter() != sleepAtChapterEnd_))) {
        sleepAtChapterEnd_ = -1;
        sleep("End of " + unit() + ". Sleep timer ended.");
    }

    int autoSleep = config_.getInt(kAutoSleep, 5);
    if (!sleeping_ && autoSleep > 0 && !player_.playing() && !pendingOpen_ && !tts_.speaking() &&
        now - lastActivity_ > std::chrono::minutes(autoSleep))
        sleep("Going to sleep.");
    checkServerMark(now);
    checkReadAhead(); // last: it sees what this tick opened, and whether playback stopped
}

// ---------------------------------------------------------------- buttons

void App::onButton(ButtonEvent ev) {
    usage_[static_cast<size_t>(ev.button)][static_cast<size_t>(ev.gesture)]++;
    lastActivity_ = clock_.now();
    // A held volume key repeats; that never wakes the device or describes a key again.
    if (ev.gesture == Gesture::Repeat && (sleeping_ || keyDescriber_)) return;

    if (sleeping_) {
        // A key that wakes the device and stays down (a volume key) must not go on to
        // repeat; like a key held when Hold was switched on, it does nothing more.
        if (held_[static_cast<size_t>(ev.button)]) locked_[static_cast<size_t>(ev.button)] = true;
        wake();
        return;
    }
    if (!keyDescriber_ && (ev.button == Button::Up || ev.button == Button::Down || ev.button == Button::Talk)) return;
    // Any other active key interrupts speech. A repeat does so only when the volume moves (in
    // changeVolume), so "Maximum volume." is not cut off by the repeat that follows it.
    if (ev.gesture != Gesture::Repeat) tts_.stop();

    if (keyDescriber_) {
        describeKey(ev);
        return;
    }

    switch (ev.button) {
    case Button::Power:
        if (ev.gesture == Gesture::Short) sleep("Going to sleep.");
        else openPowerMenu();
        return;
    case Button::PlayPause:
        if (ev.gesture == Gesture::Short) togglePlay();
        else whereAmI();
        return;
    case Button::VolumeUp:
    case Button::VolumeDown:
        changeVolume(ev.button == Button::VolumeUp ? kVolumeStep : -kVolumeStep, ev.gesture);
        return;
    case Button::Options:
        openOptions();
        return;
    default:
        break;
    }

    if (mode() == Mode::Player) handlePlayer(ev);
    else handleMenu(ev);
}

// Short OK selects, held OK opens Options, and Back / Forward move in menus.
void App::handleMenu(ButtonEvent ev) {
    if (screens_.empty()) {
        goHome();
        return;
    }
    switch (ev.button) {
    case Button::Back:
    case Button::Forward:
        moveMenu(ev.button == Button::Back ? -1 : 1, ev.gesture);
        return;
    case Button::Ok:
        if (ev.gesture == Gesture::Long) openOptions();
        else activate();
        return;
    case Button::Home:
        if (ev.gesture == Gesture::Long) goHome();
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
        if (ev.gesture == Gesture::Short) {
            cancelNavigation();
            stepChapter(dir);
        } else startSeeking(ev.button, ev.gesture == Gesture::Deep);
        return;
    }
    case Button::Ok:
        if (ev.gesture == Gesture::Long) openOptions();
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
    say(keyHelp(ev.button) + (ev.button == Button::Home ? " Hold Esc now to leave key describer." : ""));
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
    if (item.value)
        if (std::string value = item.value(); !value.empty()) text += ", " + value;
    if (verbosity <= Normal)
        text += ", " + std::to_string(screen.index + 1) + " of " + std::to_string(screen.items.size());
    if (verbosity == Beginner && (item.submenu || item.action)) {
        const std::string verb = !item.okHint.empty() ? item.okHint : item.submenu ? "open" : "select";
        text += ". Press OK to " + verb + (item.options ? ", hold OK for options" : "") + ".";
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
        say(playerIdentity() + playerStatusBody() + downloading);
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
    cancelStarOptions(); // a different screen must not hide an outstanding track action
    cancelGestures();
    if (ping_) serverAt(pingLink_).cancel(std::exchange(ping_, 0)); // the user moved on from Server status
    cancelResume(); // likewise from a stream resume (resumeStream()) still loading
    cancelLoading(); // and from an album Options is loading for: a screen over the Options screen leaves it
    screen.id = ++lastScreenId_;
    screens_.push_back(std::move(screen));
}

// Every way off the stack comes here, so a screen's request never outlives it.
void App::closeScreens(size_t keep) {
    if (screens_.size() > keep) cancelGestures();
    while (screens_.size() > keep) {
        std::uint64_t id = screens_.back().id;
        if (starOptions_ == id) starOptions_.reset();
        if (auto list = remoteLists_.find(id); list != remoteLists_.end()) {
            if (list->second.request) serverAt(list->second.link).cancel(list->second.request);
            remoteLists_.erase(list);
        }
        if (ping_ && pingScreen_ == id) serverAt(pingLink_).cancel(std::exchange(ping_, 0));
        if (resume_ && resumeScreen_ == id) cancelResume(); // a server book still loading for this screen
        if (progressPullScreen_ == id) cancelProgressPull();
        if (loading_.screen == id) cancelLoading();         // an album Options is loading for
        if (playlistEdit_.screen == id) cancelPlaylistEdit();
        if (saving_.screen == id) cancelSaving();           // Save as playlist is waiting for the server
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

// Single steps follow Menu wrap; deep and held jumps clamp, regardless of sorting.
void App::moveMenu(int direction, Gesture gesture) {
    Screen& screen = screens_.back();
    if (screen.items.empty()) {
        earcons_.play(Earcon::Edge);
        say(screen.title + ", empty.");
        return;
    }
    const long count = long(screen.items.size());
    const long from = long(screen.index);
    long target;
    bool wrapped = false;
    if (gesture == Gesture::Long) {
        target = direction < 0 ? 0 : count - 1;
    } else if (gesture == Gesture::Deep) {
        target = std::clamp(from + direction * kMenuJump, 0L, count - 1);
    } else {
        target = from + direction;
        if (target < 0 || target >= count) {
            if (count > 1 && config_.getInt(kMenuWrap, 1)) {
                target = (target + count) % count;
                wrapped = true;
            } else target = from;
        }
    }
    screen.index = size_t(target);
    earcons_.play(wrapped ? Earcon::Wrap : target == from ? Earcon::Edge : Earcon::Move);
    std::string text = itemSpeech(screen);
    auto list = remoteLists_.find(screen.id);
    if (gesture != Gesture::Short && (target == 0 || target == count - 1) &&
        list != remoteLists_.end() && list->second.more && !list->second.waiting &&
        list->second.error == MusicServer::Error::None)
        text += ". More items are available. Load more to continue.";
    say(text);
}

void App::back() {
    if (screens_.size() <= 1) {
        if (book_) {
            enterPlayer();
            return;
        }
        earcons_.play(Earcon::Edge);
        if (!screens_.empty()) say(screens_.back().title + ". " + itemSpeech(screens_.back()));
        return;
    }
    popScreen();
    earcons_.play(Earcon::Close);
    const Screen& screen = screens_.back();
    say(screen.isPlayer() ? nowPlayingSpeech() : itemSpeech(screen));
}

void App::popScreen() {
    if (screens_.size() <= 1) return; // Home stays
    closeScreens(screens_.size() - 1);
    Screen& screen = screens_.back();
    if (screen.isPlayer() || !screen.build) return; // a list from the server keeps what loaded
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

// Return to the player screen if it is on the stack, else push the screen of the loaded player.
// Only the loaded player has a screen on the stack: a different player is started from a menu.
void App::enterPlayer() {
    if (!book_) {
        goHome();
        return;
    }
    if (auto player = playerScreenIndex()) {
        closeScreens(*player + 1);
        earcons_.play(Earcon::Close);
    } else {
        pushScreen(playerScreen(music_));
        earcons_.play(Earcon::Open);
    }
    say(nowPlayingSpeech());
}

std::optional<size_t> App::playerScreenIndex() const {
    auto player = std::find_if(screens_.begin(), screens_.end(), [](const Screen& s) { return s.isPlayer(); });
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

void App::saveConfig() {
    volumeChanged_ = false;
    config_.save();
}

// Written once here instead of on every pause, book change and 30 s of playback,
// to spare the flash. A crash or power loss loses the place since the last exit.
void App::saveOnExit() {
    rememberPosition();
    pushProgress();
    if (volumeChanged_) saveConfig();
    saveState();
    saveQueue();
    index_.savePlayTimes(); // the only time play times are written; false: the next exit tries again
    trimCache(true); // the manifests are written on exit too, not at every play (a second call at power off usually finds nothing to do)
}

// The music queue, loaded or parked, in queue.tsv: the file is replaced only when its text changes (a queue
// that is paused where it was writes nothing). The stream URLs of its tracks are made when they play and are
// never in it.
void App::saveQueue() {
    if (queueFile_.empty() || !textFiles_) return;
    std::string text;
    if (book_ && music_) text = formatQueue(SavedQueue{book_->title, queue_, place()});
    else if (parked_ && parked_->music) text = formatQueue(SavedQueue{parked_->book.title, parked_->queue, parked_->position});
    if (text == queueText_) return;
    if (textFiles_->writeAtomically(queueFile_, text)) queueText_ = text; // else the next exit tries again
}

// A file that is not there, is damaged, is not ours or is too long to read gives no queue. It is replaced by
// the next queue, or by an empty file if there is none, so it does not stay for ever.
void App::restoreQueue() {
    restoredQueue_ = false;
    queueText_.clear();
    if (queueFile_.empty() || !textFiles_) return;
    std::error_code ec;
    const auto size = std::filesystem::file_size(queueFile_, ec);
    if (ec) return;
    if (size > kMaxQueueFileBytes) {
        queueText_ = "unreadable\n"; // differs from any queue or from none: it is replaced at the next exit
        return;
    }
    const std::optional<std::string> text = textFiles_->read(queueFile_);
    if (!text) return;
    queueText_ = *text;
    std::optional<SavedQueue> saved = parseQueue(*text);
    if (!saved) return;
    retitleDownloads(saved->queue);
    state_.clearPrefix(kLastStreamPrefix); // the older way of keeping a stream; the queue says more
    Book list;
    list.title = saved->title;
    book_ = std::move(list);
    music_ = true;
    queue_ = std::move(saved->queue);
    fileIndex_ = queue_.position();
    reopenAt_ = saved->position; // closed: Play, or Now playing on Home, opens it there
    restoredQueue_ = true;
}

// An older Play all named a downloaded track by its file name, which is the server's track id, and a queue
// saved then keeps that name. Such an entry (a local file whose title is its file name) gets the title the
// index has for the download, if it has one; the rest of the queue is kept as it was.
// Only the titles change: order, place, shuffle and repeat stay as they were read. A queue with no such entry
// (any queue saved since) costs one pass over its entries.
void App::retitleDownloads(Queue& queue) const {
    if (cacheRoot_.empty()) return;
    const std::string cache = (cacheRoot_ / "").string(); // with a separator: "cache2/x" is not in "cache"
    std::vector<std::size_t> named; // original indexes of downloads named by their file names
    const std::vector<QueueEntry>& entries = queue.originalEntries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const QueueEntry& e = entries[i];
        if (e.kind == QueueEntry::Kind::Local && e.path.starts_with(cache) && e.title == chapterTitle(e.path))
            named.push_back(i);
    }
    if (named.empty()) return;
    std::map<std::string, std::string> titles; // the location of a downloaded track: its title
    for (const auto& [id, item] : index_.items())
        if (item.source == IndexItem::Source::Cached)
            for (const IndexTrack& t : item.tracks)
                if (!t.title.empty()) titles[t.location] = t.title;
    for (std::size_t i : named)
        if (auto it = titles.find(queue.originalEntries()[i].path); it != titles.end()) queue.setTitle(i, it->second);
}

// Power off writes, and then the front end's shutdown() would write the same again.
void App::saveState() {
    if (state_.values() == savedState_) return;
    if (state_.save()) savedState_ = state_.values(); // else the next exit tries again
}

MenuItem App::choice(const std::string& label, const std::string& key,
                     std::vector<std::pair<int, std::string>> options, int fallback,
                     std::function<void(int)> onChange) {
    // OK opens the whole list, starting on the current value; choosing one saves it, applies it and goes back.
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

// Home lists the player used last as "Now playing" and the other as "Continue", each when it
// has something: loaded, parked behind the other, or saved for after a restart.
std::vector<MenuItem> App::homeMenu() {
    std::vector<MenuItem> items;
    bool musicFirst = book_ ? music_ : lastPlayerIsMusic();
    for (bool music : {musicFirst, !musicFirst}) {
        std::optional<std::string> title = playerTitle(music);
        if (!title) continue;
        // Home is not rebuilt while a resumed track opens: the item then goes to the player.
        MenuItem item = action(items.empty() ? "Now playing" : "Continue", [this, music] { openPlayer(music); });
        item.value = [title = *title] { return title; };
        items.push_back(item);
    }
    items.push_back(submenu("Library", [this] { return libraryMenu(); }));
    items.push_back(submenu("Music", [this] { return musicMenu(); }));
    // One entry per server, in config order; with none set up, one that says so.
    for (size_t link = 0; link < std::max<size_t>(servers_.size(), 1); ++link)
        items.push_back(submenu(linkName(link), [this, link] { return serverMenu(link); }));
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
    // The audiobooks of this server (server.audiobook_folder), after the local ones. Only what is
    // in the index: with the server down they are listed and say "Server not reachable" on OK.
    // (startSync() has dropped what was seen on other servers.)
    std::vector<const IndexItem*> remote;
    for (const auto& [id, item] : index_.items())
        if (item.source == IndexItem::Source::Server && item.kind == IndexItem::Kind::Book) remote.push_back(&item);
    std::stable_sort(remote.begin(), remote.end(), [](const IndexItem* x, const IndexItem* y) { return naturalLess(x->title, y->title); });
    for (const IndexItem* item : remote) {
        MenuItem entry = action(item->title, [this, copy = *item] { openServerBook(copy); });
        entry.value = [] { return std::string("on the server"); };
        items.push_back(std::move(entry));
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
        if (localPlaylists_ || hasPlaylists()) items.push_back(submenu("Playlists", [this] { return playlistsMenu(); })); // downloaded ones need no scan
        items.push_back(refresh);
        return items;
    }
    if (albums.empty()) {
        items.push_back(action("No music", [this] { say("Copy music into " + musicRoot_.string() + ". One folder per album."); }));
        if (localPlaylists_ || hasPlaylists()) items.push_back(submenu("Playlists", [this] { return playlistsMenu(); })); // downloaded ones need no index
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
        if (b.files.empty()) continue;
        appendBook(all, b); // a download's titles go with its files, which are named by the server's track ids
    }
    MenuItem playAll = action("Play all", [this, all] {
        // Only the server has albums (or the scan has not found the files yet): Play all never plays the server.
        if (all.files.empty()) return say(scanRequest_ ? "Looking for music. Try again in a moment." : "No music");
        if (playMusic(all, 0)) enterPlayer();
    });
    if (!all.files.empty()) {
        playAll.value = [n = all.files.size()] { return countValue(n, "track"); };
        playAll.okHint = "play";
        playAll = withOptions(std::move(playAll), localAlbumPick(all, ""));
    }
    items.push_back(playAll);
    items.push_back(submenu("Artists", [this] { return artistsMenu(); }));
    items.push_back(submenu("Albums A to Z", [this] { return albumList(mergeAlbums(index_.items())); }));
    items.push_back(submenu("Playlists", [this] { return playlistsMenu(); }));
    items.push_back(submenu("Genres", [this] { return genresMenu(); }));
    items.push_back(submenu("Recently added", [this] { return recentlyAddedMenu(); }));
    items.push_back(submenu("Recently played", [this] { return recentlyPlayedMenu(); }));
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
        MenuItem item = submenu(a.key.empty() ? kUnknownArtist : a.name, [this, key = a.key] {
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

namespace {
// The genres of an entry, one per distinct name (case and spacing folded): a local album tagged
// "Rock" that the server files under "Alternative" is in both. Primary item first.
std::vector<std::pair<std::string, std::string>> genresOf(const MergedAlbum& album) { // (key, spelling)
    std::vector<std::pair<std::string, std::string>> genres;
    for (const IndexItem* item : album.items) {
        std::string key = artistKey(item->genre);
        if (key.empty()) continue;
        if (std::none_of(genres.begin(), genres.end(), [&](const auto& g) { return g.first == key; })) genres.emplace_back(key, item->genre);
    }
    return genres;
}

bool realTime(int64_t t) { return t >= kEarliestPlayTime; } // a clock that was never set is no time at all

// When an entry was added: the earliest real time of its items, firstSeen for a device or
// downloaded item and `created` for a server one (firstSeen when the server gave none): the
// album has been in the library since the first of them. 0: no item has a real time.
int64_t addedOf(const MergedAlbum& album) {
    int64_t added = 0;
    for (const IndexItem* item : album.items) {
        int64_t t = item->source == IndexItem::Source::Server && realTime(item->created) ? item->created : item->firstSeen;
        if (realTime(t) && (added == 0 || t < added)) added = t;
    }
    return added;
}

// When an entry was last played: the latest real time of its items. 0: never.
int64_t playedOf(const MergedAlbum& album) {
    int64_t played = 0;
    for (const IndexItem* item : album.items)
        if (realTime(item->lastPlayed)) played = std::max(played, item->lastPlayed);
    return played;
}
} // namespace

// Genres: the albums of both sources by genre, from tags and from the server. Names that differ
// only in case or spacing are one genre, spoken as its first spelling (by item id); albums with
// no genre are not listed. Sorted like artists.
std::vector<MenuItem> App::genresMenu() {
    struct Genre {
        std::string name, key;
        size_t albums = 0;
    };
    std::vector<Genre> genres;
    std::map<std::string, size_t> byKey;
    for (const MergedAlbum& m : mergeAlbums(index_.items())) {
        for (const auto& [key, name] : genresOf(m)) {
            auto [it, fresh] = byKey.emplace(key, genres.size());
            if (fresh) genres.push_back({name, key, 0});
            ++genres[it->second].albums;
        }
    }
    std::stable_sort(genres.begin(), genres.end(), [](const Genre& a, const Genre& b) { return naturalLess(a.key, b.key); });
    std::vector<MenuItem> items;
    for (const Genre& g : genres) {
        MenuItem item = submenu(g.name, [this, key = g.key] {
            std::vector<MergedAlbum> theirs;
            for (MergedAlbum& m : mergeAlbums(index_.items()))
                for (const auto& g : genresOf(m))
                    if (g.first == key) theirs.push_back(m);
            return albumList(std::move(theirs));
        });
        item.value = [n = g.albums] { return countValue(n, "album"); };
        items.push_back(item);
    }
    if (items.empty()) items.push_back(action("No genres", [this] { say("No album has a genre yet."); }));
    return items;
}

// The albums with a time (`when`; 0: none), the greatest first, ties by title, at most kRecentAlbums;
// `empty` is the one item of a list with none.
std::vector<MenuItem> App::recentAlbums(int64_t (*when)(const MergedAlbum&), const std::string& empty) {
    std::vector<std::pair<int64_t, MergedAlbum>> dated;
    for (MergedAlbum& m : mergeAlbums(index_.items()))
        if (int64_t t = when(m)) dated.emplace_back(t, std::move(m));
    std::stable_sort(dated.begin(), dated.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return naturalLess(a.second.primary->title, b.second.primary->title);
    });
    std::vector<MergedAlbum> albums;
    for (auto& d : dated) {
        if (albums.size() == kRecentAlbums) break;
        albums.push_back(std::move(d.second));
    }
    if (albums.empty()) return {action(empty, [this, empty] { say(empty + "."); })};
    return albumList(std::move(albums), false);
}

std::vector<MenuItem> App::recentlyAddedMenu() { return recentAlbums(addedOf, "No new albums"); }

// The albums played from Music, the last one first.
std::vector<MenuItem> App::recentlyPlayedMenu() { return recentAlbums(playedOf, "Nothing played yet"); }

// The play time is kept in the index in memory and written on exit and sleep (saveOnExit()).
void App::markPlayed(const std::string& itemId) {
    int64_t now = clock_.wallTime();
    if (realTime(now)) index_.markPlayed(itemId, now);
}

// The albums of `albums` as entries of a list, by title unless `byTitle` is false.
std::vector<MenuItem> App::albumList(std::vector<MergedAlbum> albums, bool byTitle) {
    if (byTitle)
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
        const std::optional<ServerRef> on = serverRefOf(item.seenOn); // the server it is of: it plays from there
        // (An item of a server that is not set up is dropped by the sync at start-up, before any list is built.)
        const size_t link = on ? on->link : 0;
        const std::string prefix = on ? LibraryIndex::seenRef(servers_[link].key, SeenKind::Album, "") : std::string(); // `<key>:album:`
        RemoteAlbum album;
        album.id = on && on->ref.starts_with(prefix) ? on->ref.substr(prefix.size()) : std::string();
        album.name = item.title;
        album.artist = item.artist;
        MenuItem entry = action(item.title, [this, album, id = item.id, link] { openRemoteAlbum(album, id, {}, link); });
        entry.okHint = "open";
        if (int n = trackCountOf(item); n > 0) entry.value = [n] { return countValue(size_t(n), "track"); };
        if (album.id.empty()) return entry;
        Pick pick = remotePick(Pick::Kind::Album, album.id, album.name, album.artist, item.id, link);
        pick.info = info;
        return withOptions(std::move(entry), std::move(pick));
    }
    Book a = bookOf(item);
    if (a.files.size() == 1 && a.files.front() == a.path) { // a loose track, not a folder
        MenuItem track = action(a.title, [this, a, id = item.id] {
            if (playMusic(a, 0)) {
                markPlayed(id);
                enterPlayer();
            }
        });
        track.okHint = "play";
        return withOptions(std::move(track), localTrackPick(a, 0, item.id));
    }
    MenuItem entry = submenu(a.title, [this, a, id = item.id] { return albumMenu(a, id); });
    entry.value = [n = a.files.size()] { return countValue(n, "track"); };
    if (a.files.empty()) return entry;
    Pick pick = localAlbumPick(a, item.id);
    pick.info = info; // where it plays from is in the album's Options
    return withOptions(std::move(entry), std::move(pick));
}

// The tracks of an album.
std::vector<MenuItem> App::albumMenu(const Book& album, const std::string& playedId) {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < album.files.size(); ++i) {
        MenuItem track = action(partTitle(album, i), [this, album, i, playedId] {
            if (!playMusic(album, i)) return;
            if (!playedId.empty()) markPlayed(playedId); // from Music; Downloaded and Streaming pass none
            enterPlayer(); // the rest of the album follows
        });
        track.okHint = "play";
        items.push_back(withOptions(std::move(track), localTrackPick(album, i, playedId)));
    }
    return items;
}

void App::openLocalAlbum(const Book& album, const std::string& playedId, size_t at) {
    openMenu(album.title, [this, album, playedId] { return albumMenu(album, playedId); }, at);
}

// ---------------------------------------------------------------- streaming

MusicServer& App::serverAt(size_t link) {
    return link < servers_.size() ? servers_[link].server : static_cast<MusicServer&>(noServer_);
}

const std::string& App::linkName(size_t link) const {
    static const std::string none = "Server"; // no server set up
    return link < servers_.size() ? servers_[link].name : none;
}

// A Books server (Audiobookshelf) has its own menu; any other server, and a link that is not usable, the Streaming one.
std::vector<MenuItem> App::serverMenu(size_t link) {
    if (serverAt(link).configured() && serverAt(link).catalogue() == MusicServer::Catalogue::Books) return booksMenu(link);
    return streamingMenu(link);
}

std::vector<MenuItem> App::booksMenu(size_t link) {
    std::vector<MenuItem> items;
    items.push_back(action("Continue listening", [this, link] { openInProgress(link); }));
    items.push_back(action("Recently added", [this, link] {
        openAlbumList(link, "Recently added", "newest", kAlbumPageSize, false, true);
    }));
    items.push_back(action("Books A to Z", [this, link] {
        openAlbumList(link, "Books A to Z", "alphabeticalByName", kAlbumPageSize, true, true);
    }));
    items.push_back(action("Authors", [this, link] { openAuthors(link); }));
    if (trackCacheOf(link).enabled()) items.push_back(submenu("Downloaded", [this, link] { return downloadedMenu(link); }));
    MenuItem status = action("Server status", [this, link] { checkServer(link); });
    status.value = [this, link] { return serverDown(link) ? std::string("not reachable") : std::string(); };
    items.push_back(std::move(status));
    return items;
}

void App::openInProgress(size_t link) {
    openRemoteList(link, "Continue listening",
        fetchPages<RemoteAlbum>(
            [this, link](int, MusicServer::Done<RemoteAlbum> done) { return serverAt(link).inProgress(std::move(done)); },
            [this, link](const std::vector<RemoteAlbum>& books) {
                std::vector<MenuItem> items;
                for (const RemoteAlbum& book : books) {
                    MenuItem item = action(book.name, [this, link, book] {
                        IndexItem saved;
                        saved.id = LibraryIndex::seenRef(servers_[link].key, SeenKind::Album, book.id);
                        saved.seenOn = {saved.id};
                        saved.title = book.name;
                        saved.artist = book.artist;
                        openServerBook(saved, true);
                    });
                    item.okHint = "play";
                    if (!book.artist.empty()) item.value = [author = book.artist] { return author; };
                    items.push_back(std::move(item));
                }
                return items;
            }),
        0, "Nothing in progress", "There are no books in progress in this library.");
}

void App::openAuthors(size_t link) {
    auto toItems = [this, link](const std::vector<RemoteArtist>& authors) {
        std::vector<MenuItem> out;
        for (const RemoteArtist& a : authors) {
            MenuItem item = action(a.name, [this, a, link] {
                openRemoteList(link, a.name,
                               fetchPages<RemoteAlbum>(
                                   [this, link, id = a.id](int, MusicServer::Done<RemoteAlbum> done) {
                                       return serverAt(link).artistAlbums(id, std::move(done));
                                   },
                                   [this, link](const std::vector<RemoteAlbum>& books) { return bookItems(books, link); }),
                               0, "No books", "The server has no books by " + a.name + ".");
            });
            item.okHint = "open";
            if (a.albumCount > 0) item.value = [n = size_t(a.albumCount)] { return countValue(n, "book"); };
            out.push_back(std::move(item));
        }
        return out;
    };
    openRemoteList(link, "Authors",
                   fetchPages<RemoteArtist>(
                       [this, link](int, MusicServer::Done<RemoteArtist> done) { return serverAt(link).artists(std::move(done)); },
                       toItems),
                   0, "No authors", "The server has no authors yet. Add books to the server, then come back.");
}

// "Title, Author": the author is the item's value, spoken after the title.
std::vector<MenuItem> App::bookItems(const std::vector<RemoteAlbum>& books, size_t link) {
    std::vector<MenuItem> items;
    for (const RemoteAlbum& b : books) {
        MenuItem item = action(b.name, [this, b, link] { openBookPage(b, link); });
        item.okHint = "open";
        if (!b.artist.empty()) item.value = [author = b.artist] { return author; };
        items.push_back(std::move(item));
    }
    return items;
}

// The book's page loads the book (files and chapters) like any list from the server.
void App::openBookPage(const RemoteAlbum& book, size_t link) {
    openRemoteList(link, book.name,
                   [this, link, book](int, PageDone done) {
                       return serverAt(link).book(book.id, [this, link, book, done](MusicServer::Error error, RemoteBook remote) {
                           done(error, error == MusicServer::Error::None ? bookPage(book, remote, link) : std::vector<MenuItem>{}, false);
                       });
                   },
                   0, "No chapters", "This book has no chapters on the server.");
}

// Play book, Download book where downloads can work, then the chapters: the server's, else one per file.
// The book plays under its Library id (`<server key>:album:<id>`), so both share the place.
std::vector<MenuItem> App::bookPage(const RemoteAlbum& album, const RemoteBook& remote, size_t link) {
    if (remote.tracks.empty()) return {};
    const std::string ref = LibraryIndex::seenRef(servers_[link].key, SeenKind::Album, album.id);
    auto book = std::make_shared<const Book>(serverBookOf(ref, album.name, album.artist, album.id, remote.tracks, link, remote.chapters));
    auto play = [this, book](size_t file, double position, bool explicitPlace = false) {
        std::uint64_t screen = screens_.back().id;
        playServerBook(*book, file, position, [this, screen] { enterPlayerFrom(screen); }, explicitPlace);
    };
    std::vector<MenuItem> items;
    MenuItem all = action("Play book", [this, play, book] {
        if (book_) rememberPosition(); // the place of this book, if it is the one playing, is saved only now
        auto [file, pos] = savedPlace(*book);
        play(file, pos);
    });
    const size_t chapters = book->chapters.empty() ? book->files.size() : book->chapters.size();
    all.value = [chapters] { return countValue(chapters, "chapter"); };
    items.push_back(std::move(all));
    // What is on the disk decides: a book whose every file is there has nothing to download (a file the server has
    // added since is one to fetch), and any download can be deleted.
    const RemoteSource source{RemoteSource::Kind::Album, album.id};
    const TrackCache& store = trackCacheOf(link);
    const CachedList cached = store.enabled() ? store.load(source) : CachedList{};
    const std::vector<RemoteTrack> onDisk = completeTracks(store, source, cached);
    const bool whole = isWholeDownload(cached, onDisk) &&
                       std::all_of(remote.tracks.begin(), remote.tracks.end(), [&](const RemoteTrack& t) {
                           return std::any_of(onDisk.begin(), onDisk.end(), [&](const RemoteTrack& d) { return d.id == t.id; });
                       });
    if (canDownloadFrom(link) && !whole)
        items.push_back(action("Download book", [this, link, album, shared = std::make_shared<const RemoteBook>(remote)] {
            startDownload(link, album.name, album.artist, shared->tracks, {RemoteSource::Kind::Album, album.id}, {}, "file", shared->chapters);
        }));
    if (!onDisk.empty()) items.push_back(deleteDownloadItem(source, album.name, link));
    auto chapter = [&](std::string label, size_t file, double start) {
        MenuItem item = action(std::move(label), [play, file, start] { play(file, start, true); });
        item.okHint = "play";
        items.push_back(std::move(item));
    };
    if (!book->chapters.empty()) {
        for (const Book::Chapter& c : book->chapters) chapter(c.title, c.file, c.start);
    } else {
        for (size_t i = 0; i < book->files.size(); ++i)
            chapter(book->titles[i].empty() ? "Chapter " + std::to_string(i + 1) : book->titles[i], i, 0);
    }
    return items;
}

std::vector<MenuItem> App::streamingMenu(size_t link) {
    if (!serverAt(link).configured()) {
        if (serverConfigured(config_)) // set up, but not as a server this player can use
            return {action("Server not supported", [this] { say(kUnsupportedServerHelp); })};
        return {action("No server set up", [this] { say(kNoServerHelp); })};
    }
    std::vector<MenuItem> items;
    items.push_back(action("Recently added", [this, link] {
        openAlbumList(link, "Recently added", "newest", kAlbumPageSize, false);
    }));
    items.push_back(action("Albums A to Z", [this, link] {
        openAlbumList(link, "Albums A to Z", "alphabeticalByName", kAlbumPageSize, true);
    }));
    items.push_back(action("Artists", [this, link] {
        auto toItems = [this, link](const std::vector<RemoteArtist>& artists) {
            std::vector<MenuItem> out;
            for (const RemoteArtist& a : artists) {
                MenuItem item = action(a.name, [this, a, link] {
                    openRemoteList(link, a.name,
                                   fetchPages<RemoteAlbum>(
                                       [this, link, id = a.id](int, MusicServer::Done<RemoteAlbum> done) {
                                           return serverAt(link).artistAlbums(id, std::move(done));
                                       },
                                       [this, link](const std::vector<RemoteAlbum>& albums) { return albumItems(albums, false, link); }),
                                   0, "No albums", "The server has no albums by " + a.name + ".");
                });
                item.okHint = "open";
                if (a.albumCount > 0) item.value = [n = size_t(a.albumCount)] { return countValue(n, "album"); };
                out.push_back(withOptions(std::move(item), remotePick(Pick::Kind::Artist, a.id, a.name, "", {}, link)));
            }
            return out;
        };
        openRemoteList(link, "Artists",
                       fetchPages<RemoteArtist>(
                           [this, link](int, MusicServer::Done<RemoteArtist> done) { return serverAt(link).artists(std::move(done)); },
                           toItems),
                       0, "No artists", "The server has no music yet. Add music to the server, then come back.");
    }));
    items.push_back(action("Random albums", [this, link] {
        openAlbumList(link, "Random albums", "random", kRandomAlbums, false);
    }));
    if (serverAt(link).supportsStars()) items.push_back(action("Starred", [this, link] {
        openRemoteList(link, "Starred", fetchPages<RemoteTrack>(
            [this, link](int, MusicServer::Done<RemoteTrack> done) { return serverAt(link).starredTracks(std::move(done)); },
            [this, link](const std::vector<RemoteTrack>& tracks) {
                auto list = std::make_shared<const std::vector<RemoteTrack>>(tracks);
                std::vector<MenuItem> out;
                for (size_t i = 0; i < list->size(); ++i) {
                    Pick pick;
                    pick.make = [this, list, i, link] { return remoteSelection((*list)[i].title, {(*list)[i]}, std::nullopt, "", false, "", link); };
                    MenuItem item = action((*list)[i].title, [this, list, i, link] { playRemote("Starred", *list, i, {}, 0, "", link); });
                    item.okHint = "play";
                    out.push_back(withOptions(std::move(item), std::move(pick)));
                }
                return out;
            }), 0, "No starred tracks", "Star a track from Options in the music player, then come back.");
    }));
    if (trackCacheOf(link).enabled()) items.push_back(submenu("Downloaded", [this, link] { return downloadedMenu(link); }));
    MenuItem status = action("Server status", [this, link] { checkServer(link); });
    status.value = [this, link] { return serverDown(link) ? std::string("not reachable") : std::string(); };
    items.push_back(std::move(status));
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
// following pages are requested only through the Load more item.
void App::openRemoteList(size_t link, const std::string& title, FetchPage fetch, int pageSize, std::string emptyLabel,
                         std::string emptyHelp, std::string focusKey) {
    pushScreen({Screen::Kind::Menu, title, {}, {}}); // no build: popScreen() keeps what has loaded
    std::uint64_t id = screens_.back().id;
    RemoteList& list = remoteLists_[id];
    list.fetch = std::move(fetch);
    list.link = link;
    list.pageSize = pageSize;
    list.emptyLabel = std::move(emptyLabel);
    list.emptyHelp = std::move(emptyHelp);
    list.focusKey = std::move(focusKey);
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
    size_t focus = first; // where the user lands: the first new item, or the one the list was opened for
    if (error != MusicServer::Error::None) {
        list.error = error;
    } else {
        sendScrobbles(); // the server answers
        size_t count = items.size();
        for (size_t i = 0; i < count && !list.focusKey.empty(); ++i)
            if (items[i].key == list.focusKey) {
                focus = first + i;
                list.focusKey.clear();
            }
        screen->items.insert(screen->items.begin() + std::ptrdiff_t(list.loaded), std::make_move_iterator(items.begin()),
                             std::make_move_iterator(items.end()));
        list.loaded += count;
        // The next page starts after what came, whatever size the server sends.
        list.more = list.pageSize > 0 && more && count > 0;
        list.offset += int(count);
    }
    showStatus(screenId);
    if (onLoading) screen->index = std::min(focus, screen->items.size() - 1);
    // Filled silently when the user has moved on to another screen, or sleeps.
    if (!onLoading || &screens_.back() != screen || sleeping_) return;
    tts_.stop();
    if (error != MusicServer::Error::None) earcons_.play(Earcon::Error);
    say(itemSpeech(*screen));
}

// After what has loaded comes one of: the error and "Try again", "Loading", "Load more", or
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
        if (hasDownloads(list.link)) items.push_back(submenu("Downloaded", [this, link = list.link] { return downloadedMenu(link); }));
    } else if (list.waiting) {
        items.push_back(action("Loading", [this] { say("Still loading."); }));
    } else if (list.more) {
        items.push_back(action("Load more", [this, screenId] {
            auto next = remoteLists_.find(screenId);
            Screen* screen = findScreen(screenId);
            if (next == remoteLists_.end() || !screen || next->second.waiting || !next->second.more) return;
            requestPage(screenId, next->second.offset, true);
            showStatus(screenId);
            screen->index = screen->items.size() - 1;
            say(itemSpeech(*screen));
        }));
    } else if (list.loaded == 0) {
        items.push_back(action(list.emptyLabel, [this, help = list.emptyHelp] { say(help); }));
    }
    screen->index = std::min(screen->index, items.size() - 1);
}

void App::openAlbumList(size_t link, const std::string& title, const std::string& listType, int pageSize, bool paged, bool books) {
    openRemoteList(link, title,
                   fetchPages<RemoteAlbum>(
                       [this, link, listType, pageSize](int offset, MusicServer::Done<RemoteAlbum> done) {
                           return serverAt(link).albums(listType, offset, pageSize, std::move(done));
                       },
                       [this, link, books](const std::vector<RemoteAlbum>& albums) {
                           return books ? bookItems(albums, link) : albumItems(albums, true, link);
                       }),
                   paged ? pageSize : 0, books ? "No books" : "No albums",
                   books ? "The server has no books yet. Add books to the server, then come back."
                         : "The server has no music yet. Add music to the server, then come back.");
}

void App::openRemoteAlbum(const RemoteAlbum& album, const std::string& playedId, const std::string& focusTrack,
                          size_t link) {
    openRemoteList(link, album.name,
                   fetchPages<RemoteTrack>(
                       [this, link, id = album.id](int, MusicServer::Done<RemoteTrack> done) {
                           return serverAt(link).albumTracks(id, std::move(done));
                       },
                       [this, name = album.name, artist = album.artist, id = album.id, playedId, link](const std::vector<RemoteTrack>& tracks) {
                           return trackItems(name, tracks, "Play album", {RemoteSource::Kind::Album, id}, artist, std::nullopt, playedId, link);
                       }),
                   0, "No tracks", "This album has no tracks on the server.", focusTrack);
}

// `withArtist`: lists of many artists say whose album it is; one artist's list says how long.
std::vector<MenuItem> App::albumItems(const std::vector<RemoteAlbum>& albums, bool withArtist, size_t link) {
    std::vector<MenuItem> items;
    for (const RemoteAlbum& a : albums) {
        MenuItem item = action(a.name, [this, a, link] { openRemoteAlbum(a, {}, {}, link); });
        item.okHint = "open";
        if (withArtist && !a.artist.empty()) item.value = [artist = a.artist] { return artist; };
        else if (!withArtist && a.songCount > 0) item.value = [n = size_t(a.songCount)] { return countValue(n, "track"); };
        items.push_back(withOptions(std::move(item), remotePick(Pick::Kind::Album, a.id, a.name, a.artist, {}, link)));
    }
    return items;
}

// "Play album" first, then "Download album" where downloads can work, then the
// tracks: OK on one plays from it to the end, like Music.
std::vector<MenuItem> App::trackItems(const std::string& title, const std::vector<RemoteTrack>& tracks,
                                      const std::string& playAll, const RemoteSource& source,
                                      const std::string& artist, std::optional<MenuItem> second,
                                      const std::string& playedId, size_t link) {
    if (tracks.empty() && (source.kind != RemoteSource::Kind::Playlist || !localPlaylists_)) return {};
    auto play = [this, title, artist, source, playedId, link](const std::shared_ptr<const std::vector<RemoteTrack>>& list, size_t from) {
        if (playRemote(title, *list, from, source, 0, artist, link) && !playedId.empty()) markPlayed(playedId);
    };
    auto list = std::make_shared<const std::vector<RemoteTrack>>(tracks); // one copy for all the items
    std::vector<MenuItem> items;
    MenuItem all = action(playAll, [this, play, list] { if (list->empty()) say("No tracks."); else play(list, 0); });
    all.value = [n = tracks.size()] { return countValue(n, "track"); };
    items.push_back(all);
    if (second) {
        items.push_back(std::move(*second));
    } else if (canDownloadFrom(link)) {
        const char* what = source.kind == RemoteSource::Kind::Album ? "Download album" : "Download playlist";
        items.push_back(action(what, [this, title, artist, list, source, link] { startDownload(link, title, artist, *list, source); }));
    }
    for (size_t i = 0; i < tracks.size(); ++i) {
        std::string label = tracks[i].title.empty() ? "Track " + std::to_string(i + 1) : tracks[i].title;
        MenuItem track = action(label, [play, list, i] { play(list, i); });
        track.okHint = "play";
        track.key = tracks[i].id;
        Pick pick;
        pick.make = [this, title, artist, source, list, i, label, playedId, link] {
            const RemoteTrack& t = (*list)[i];
            Selection selection = remoteSelection(title, {t}, std::nullopt, artist, source.kind == RemoteSource::Kind::Album,
                                                  source.kind == RemoteSource::Kind::Album ? source.id : std::string(), link);
            selection.title = selection.list.title = label;
            selection.playedId = playedId;
            return selection;
        };
        track = withOptions(std::move(track), std::move(pick));
        if (localPlaylists_ && source.kind == RemoteSource::Kind::Playlist && serverAt(link).supportsPlaylistEditing()) {
            auto options = track.options;
            track.options = [this, options, source, title, link] {
                auto result = options();
                result.push_back(action("Remove from playlist", [this, source, title, link] {
                    selectServerPlaylistRemoval({link, source.id, title, 0, false});
                }));
                return result;
            };
        }
        items.push_back(std::move(track));
    }
    if (source.kind == RemoteSource::Kind::Playlist && canDownloadFrom(link)) {
        const bool kept = state_.getInt(keptPlaylistKey(link, source.id), 0) != 0;
        items.push_back(action(kept ? "Stop keeping offline" : "Keep offline", [this, link, source, title] {
            const bool kept = state_.getInt(keptPlaylistKey(link, source.id), 0) != 0;
            if (!setPlaylistKept(link, source.id, title, !kept)) {
                earcons_.play(Earcon::Error); say("Cannot write the download."); return;
            }
            if (!screens_.empty()) {
                for (auto& item : screens_.back().items)
                    if (item.label == (kept ? "Stop keeping offline" : "Keep offline")) {
                        item.label = kept ? "Keep offline" : "Stop keeping offline";
                        break;
                    }
            }
            say(kept ? "Stopped keeping offline." : "Keeping offline.");
        }));
    }
    if (localPlaylists_ && source.kind == RemoteSource::Kind::Playlist && serverAt(link).supportsPlaylistEditing())
        items.push_back(deletePlaylistItem({link, source.id, title, tracks.size(), false}));
    return items;
}

// ---------------------------------------------------------------- options

namespace {
// What the actions of Options are called, and what they do (docs/queue.md, "Filling it").
constexpr const char* kPlayNow = "Play now";
constexpr const char* kPlayNext = "Play next";
constexpr const char* kAddToQueue = "Add to queue";
constexpr const char* kDownloadAlbum = "Download album";
constexpr const char* kDownloadPlaylist = "Download playlist";

bool sameList(const RemoteSource& a, const RemoteSource& b) { return a.kind == b.kind && a.id == b.id; }

bool qualityAtLeast(int cached, int requested) {
    return cached == 0 || (requested > 0 && cached >= requested);
}

// A list the user downloaded whole: every manifest track pinned and complete on disk (`onDisk` are the
// complete ones), and its download over (a failed or cut-short one has only the tracks that arrived).
// It plays from the disk at once, with no question to the server.
bool isWholeDownload(const CachedList& list, const std::vector<RemoteTrack>& onDisk) {
    return !list.tracks.empty() && onDisk.size() == list.tracks.size() &&
           std::all_of(list.tracks.begin(), list.tracks.end(),
                       [](const CachedTrack& t) { return t.pinned && t.changed != kPartialMark; });
}
// "Weather, 12 tracks": a single track is named alone.
std::string selectionText(const std::string& title, size_t tracks) {
    return tracks == 1 ? title : title + ", " + countValue(tracks, "track");
}
} // namespace

// Options on a menu item or the playing track; a book offers its later server place.
// With no available actions, say "No options" with the edge sound.
void App::openOptions() {
    if (mode() == Mode::Player && music_ && book_ && !queue_.empty()) {
        openMenu("Options, " + trackTitle(fileIndex_), [this] { return playerOptionsMenu(); });
        loadPlayerStar();
        return;
    }
    if (mode() == Mode::Player && !music_ && book_ && serverPlace_) {
        openMenu("Options, " + book_->title, [this] {
            if (!serverPlace_) return std::vector<MenuItem>{};
            const ServerPlace place = *serverPlace_;
            return std::vector<MenuItem>{action("Go to the server's place, chapter " + std::to_string(place.chapter + 1) +
                ", " + spokenDuration(place.progress.seconds), [this, place] {
                    if (!serverPlace_) return;
                    if (auto player = playerScreenIndex()) closeScreens(*player + 1);
                    useServerPlace(place);
                })};
        });
        return;
    }
    if (mode() == Mode::Menu && !screens_.empty()) {
        const Screen& screen = screens_.back();
        if (!screen.items.empty() && screen.items[screen.index].options) {
            const MenuItem item = screen.items[screen.index]; // copy: opening pushes a screen
            openMenu("Options, " + item.label, item.options);
            return;
        }
    }
    earcons_.play(Earcon::Edge);
    say("No options.");
}

// Keep the ordinary actions usable while the authoritative star state loads. The request
// belongs to this Options screen, so closeScreens cancels both reads and mutations.
void App::cancelStarOptions() {
    if (!starOptions_) return;
    for (size_t i = 0; i < screens_.size(); ++i)
        if (screens_[i].id == *starOptions_) { closeScreens(i); return; }
    starOptions_.reset();
}

// Star routing is independent of playback routing: a known local copy stays local.
// Only the playing IndexTrack's exact track references identify it on a server. The
// outer loop deliberately follows configured-server order, rather than reference order.
std::optional<App::StarTarget> App::starTargetOf(const QueueEntry& entry) const {
    if (entry.kind == QueueEntry::Kind::Remote) {
        auto link = linkOf(entry);
        if (link && servers_[*link].server.supportsStars() && !entry.trackId.empty())
            return StarTarget{*link, entry.trackId};
        return std::nullopt;
    }
    const auto hit = index_.findByLocation(entry.path);
    if (!hit.track) return std::nullopt;
    for (size_t link = 0; link < servers_.size(); ++link) {
        const auto& server = servers_[link];
        if (server.key.empty() || !server.server.configured() || !server.server.supportsStars()) continue;
        const std::string prefix = LibraryIndex::seenRef(server.key, SeenKind::Track, "");
        for (const auto& ref : hit.track->seenOn)
            if (ref.starts_with(prefix) && ref.size() > prefix.size())
                return StarTarget{link, ref.substr(prefix.size())};
    }
    return std::nullopt;
}

void App::loadPlayerStar() {
    const QueueEntry* entry = queue_.current();
    if (!entry) return;
    const auto target = starTargetOf(*entry);
    if (!target) return;
    const auto screen = screens_.back().id;
    starOptions_ = screen;
    screens_.back().build = {}; // this screen keeps its asynchronously loaded actions
    const std::string trackId = target->trackId;
    RemoteList& list = remoteLists_[screen];
    list.link = target->link;
    list.loaded = screens_.back().items.size();
    list.fetch = [this, screen, link = target->link, trackId](int, PageDone done) {
        return serverAt(link).trackDetails(trackId, [this, screen, link, trackId, done](MusicServer::Error error,
                std::vector<RemoteTrack> tracks, bool) {
            if (error == MusicServer::Error::None && (tracks.size() != 1 || tracks.front().id != trackId))
                error = MusicServer::Error::Server;
            std::vector<MenuItem> items;
            if (error == MusicServer::Error::None) {
                const bool starred = tracks.front().starred;
                items.push_back(action(starred ? "Unstar" : "Star", [this, screen, link, trackId, starred] {
                    changeStar(screen, link, trackId, !starred);
                }));
            }
            const Screen* target = findScreen(screen);
            const bool onLoading = target && target->index >= remoteLists_.at(screen).loaded;
            done(error, std::move(items), false);
            if (error != MusicServer::Error::None && !onLoading && !screens_.empty() &&
                screens_.back().id == screen && !sleeping_) {
                earcons_.play(Earcon::Error);
                say(errorText(error));
            }
        });
    };
    requestPage(screen, 0, true);
    showStatus(screen);
}

void App::changeStar(std::uint64_t screenId, size_t link, std::string trackId, bool starred) {
    auto it = remoteLists_.find(screenId);
    if (it == remoteLists_.end()) return;
    if (it->second.request) { say("Still saving."); return; }
    say("Saving.");
    it->second.request = serverAt(link).setStarred(std::move(trackId), starred, [this, screenId, starred](MusicServer::Error error) {
        auto found = remoteLists_.find(screenId);
        if (found == remoteLists_.end()) return;
        found->second.request = 0;
        if (screens_.empty() || screens_.back().id != screenId || sleeping_) return;
        if (error != MusicServer::Error::None) {
            earcons_.play(Earcon::Error);
            say(errorText(error));
            return;
        }
        popScreen();
        earcons_.play(Earcon::Close);
        say(starred ? "Starred." : "Unstarred.");
    });
}

std::vector<MenuItem> App::playerOptionsMenu() {
    return {
        action("Go to album", [this] { goToAlbum(); }),
        action("Album info", [this] { sayPlayerInfo(); }),
    };
}

// Leaves the Options screen and opens the album of the playing track, on that track: a local one from the
// index, a server one loaded from the server. A track whose album is not known says so and stays.
void App::goToAlbum() {
    const QueueEntry* entry = queue_.current();
    if (!entry) return;
    const QueueEntry track = *entry; // opening a screen changes nothing here, but a copy is cheap and safe
    if (track.kind == QueueEntry::Kind::Local) {
        const IndexItem* item = index_.findByLocation(track.path).item;
        Book album = item ? bookOf(*item) : Book{};
        const bool loose = album.files.size() == 1 && album.files.front() == album.path; // a track, not a folder
        if (!item || loose) {
            earcons_.play(Earcon::Error);
            say("No album for this track.");
            return;
        }
        size_t at = 0;
        for (size_t i = 0; i < album.files.size(); ++i)
            if (album.files[i] == track.path) at = i;
        const std::string playedId = item->id; // before the screens change
        popScreen();
        openLocalAlbum(album, playedId, at);
        return;
    }
    if (availability(fileIndex_, Origin::Menu) != Availability::Yes || track.albumId.empty()) {
        earcons_.play(Earcon::Error);
        say("No album for this track.");
        return;
    }
    const size_t link = linkOf(track).value_or(0); // availability() found it
    const IndexItem* item = index_.findBySeen(LibraryIndex::seenRef(servers_[link].key, SeenKind::Album, track.albumId));
    RemoteAlbum album;
    album.id = track.albumId;
    album.name = track.album;
    album.artist = track.artist;
    const std::string playedId = item ? item->id : std::string();
    popScreen();
    openRemoteAlbum(album, playedId, track.trackId, link);
}

MenuItem App::withOptions(MenuItem item, Pick pick) {
    item.options = [this, pick = std::move(pick)] { return optionsMenu(pick); };
    return item;
}

std::vector<MenuItem> App::optionsMenu(const Pick& pick) {
    std::vector<MenuItem> items = {
        action(kPlayNow, [this, pick] { choose(pick, QueueAction::PlayNow); }),
        action(kPlayNext, [this, pick] { choose(pick, QueueAction::PlayNext); }),
        action(kAddToQueue, [this, pick] { choose(pick, QueueAction::AddToQueue); }),
    };
    if (offersDownload(pick))
        items.push_back(action(pick.kind == Pick::Kind::Album ? kDownloadAlbum : kDownloadPlaylist,
                               [this, pick] { choose(pick, QueueAction::Download); }));
    // An album also says where it plays from; that only reports, so the Options screen stays.
    if (!pick.info.empty()) items.push_back(action("Album info", [this, info = pick.info] { say(info); }));
    return items;
}

RemoteSource App::sourceOf(const Pick& pick) {
    return {pick.kind == Pick::Kind::Album ? RemoteSource::Kind::Album : RemoteSource::Kind::Playlist, pick.id};
}

// Download is offered for an album or playlist of the server where downloads work, unless it is already
// whole at the requested quality (every track pinned and complete: nothing is left to fetch).
bool App::offersDownload(const Pick& pick) const {
    if (pick.kind != Pick::Kind::Album && pick.kind != Pick::Kind::Playlist) return false;
    if (!canDownloadFrom(pick.link)) return false;
    if (pick.kind == Pick::Kind::Playlist && state_.getInt(stalePlaylistKey({pick.link, pick.id, pick.title, 0, false}), 0)) return true;
    const TrackCache& store = trackCacheOf(pick.link);
    const RemoteSource source = sourceOf(pick);
    if (downloading(source, pick.link)) return true; // its manifest holds only what has arrived
    const CachedList cached = store.load(source);
    if (!isWholeDownload(cached, completeTracks(store, source, cached))) return true;
    const int quality = servers_[pick.link].server.catalogue() == MusicServer::Catalogue::Books ? 0 :
                        std::max(0, config_.getInt(kDownloadBitrate, kOriginalQuality));
    return std::any_of(cached.tracks.begin(), cached.tracks.end(),
                       [quality](const CachedTrack& track) { return !qualityAtLeast(track.bitrate, quality); });
}

// One track of a local list, alone. The queue is called after the track, as a loose track is.
App::Pick App::localTrackPick(const Book& album, size_t track, const std::string& playedId) {
    Pick pick;
    pick.make = [album, track, playedId] {
        Selection selection;
        selection.title = partTitle(album, track);
        selection.list.title = selection.title;
        selection.list.path = album.files[track];
        selection.list.files = {album.files[track]};
        selection.entries = {entriesOf(album)[track]};
        selection.playedId = playedId;
        return selection;
    };
    return pick;
}

App::Pick App::localAlbumPick(const Book& album, const std::string& playedId) {
    Pick pick;
    pick.make = [album, playedId] { return Selection{album.title, album, entriesOf(album), playedId}; };
    return pick;
}

App::Pick App::remotePick(Pick::Kind kind, const std::string& id, const std::string& title, const std::string& artist,
                          const std::string& playedId, size_t link) {
    Pick pick;
    pick.link = link;
    pick.kind = kind;
    pick.id = id;
    pick.title = title;
    pick.artist = artist;
    pick.playedId = playedId;
    return pick;
}

App::Selection App::remoteSelection(const std::string& title, const std::vector<RemoteTrack>& tracks,
                                    std::optional<RemoteSource> source, const std::string& artist, bool ofAlbum,
                                    const std::string& albumId, size_t link) const {
    Selection selection;
    selection.title = title;
    selection.list = remoteBook(title, tracks, std::move(source), artist, link);
    selection.entries = remoteEntries(title, tracks, ofAlbum, artist, albumId, link);
    return selection;
}

// A Ready pick acts at once. A remote album, playlist or artist is loaded first ("Loading."); the answer
// finds its Options screen by id, and leaving it cancels the request (closeScreens()).
void App::choose(const Pick& pick, QueueAction action) {
    cancelLoading(); // pressed again while it loads
    if (pick.kind == Pick::Kind::Ready) {
        perform(pick.make(), action);
        return;
    }
    if (action == QueueAction::Download && download_) { // nothing to load: the progress, or the refusal
        sayDownloadBusy(sourceOf(pick), pick.link);
        return;
    }
    const std::uint64_t screen = screens_.back().id;
    loading_ = Loading{screen, 0, pick, action, {}, 0, {}};
    // A list that is whole on the disk needs no question to the server (it may be down, or the list gone from it).
    if (action != QueueAction::Download && (pick.kind == Pick::Kind::Album || pick.kind == Pick::Kind::Playlist)) {
        const TrackCache& store = trackCacheOf(pick.link); // of the pick's server: another server's ids could match
        const RemoteSource source = sourceOf(pick);
        const CachedList cached = store.enabled() ? store.load(source) : CachedList{};
        std::vector<RemoteTrack> onDisk = completeTracks(store, source, cached);
        const bool stale = pick.kind == Pick::Kind::Playlist &&
            state_.getInt(stalePlaylistKey({pick.link, pick.id, pick.title, 0, false}), 0);
        if (!stale && isWholeDownload(cached, onDisk)) {
            onPickLoaded(screen, MusicServer::Error::None, std::move(onDisk));
            return;
        }
    }
    say("Loading.");
    switch (pick.kind) {
    case Pick::Kind::Album:
        loading_.request = serverAt(pick.link).albumTracks(pick.id, [this, screen](MusicServer::Error error, std::vector<RemoteTrack> tracks, bool) {
            onPickLoaded(screen, error, std::move(tracks));
        });
        break;
    case Pick::Kind::Playlist:
        loading_.request = serverAt(pick.link).playlistTracks(pick.id, [this, screen](MusicServer::Error error, std::vector<RemoteTrack> tracks, bool) {
            onPickLoaded(screen, error, std::move(tracks));
        });
        break;
    case Pick::Kind::Artist:
        loading_.request = serverAt(pick.link).artistAlbums(pick.id, [this, screen](MusicServer::Error error, std::vector<RemoteAlbum> albums, bool) {
            onArtistAlbums(screen, error, std::move(albums));
        });
        break;
    case Pick::Kind::Ready: break;
    }
}

void App::onArtistAlbums(std::uint64_t screen, MusicServer::Error error, std::vector<RemoteAlbum> albums) {
    if (loading_.screen != screen) return;
    loading_.request = 0;
    if (error != MusicServer::Error::None) return failLoading(error);
    loading_.albums = std::move(albums);
    requestNextAlbum();
}

// An artist's tracks, album after album: the server has no call for all of them.
void App::requestNextAlbum() {
    const std::uint64_t screen = loading_.screen;
    if (loading_.next < loading_.albums.size()) {
        loading_.request = serverAt(loading_.pick.link).albumTracks(loading_.albums[loading_.next].id,
                                               [this, screen](MusicServer::Error error, std::vector<RemoteTrack> tracks, bool) {
                                                   if (loading_.screen != screen) return;
                                                   if (error == MusicServer::Error::None) {
                                                       loading_.tracks.insert(loading_.tracks.end(), std::make_move_iterator(tracks.begin()),
                                                                              std::make_move_iterator(tracks.end()));
                                                       ++loading_.next;
                                                       loading_.request = 0;
                                                       requestNextAlbum();
                                                   } else {
                                                       loading_.request = 0;
                                                       failLoading(error);
                                                   }
                                               });
        return;
    }
    std::vector<RemoteTrack> tracks = std::move(loading_.tracks);
    loading_.tracks.clear();
    onPickLoaded(screen, MusicServer::Error::None, std::move(tracks));
}

void App::onPickLoaded(std::uint64_t screen, MusicServer::Error error, std::vector<RemoteTrack> tracks) {
    if (loading_.screen != screen) return;
    if (error != MusicServer::Error::None) return failLoading(error);
    sendScrobbles(); // the server answers
    const Loading done = std::exchange(loading_, Loading{});
    const Pick& pick = done.pick;
    if (done.action == QueueAction::Download) {
        tts_.stop(); // "Loading." is over
        if (tracks.empty()) { // stay on the Options, as perform() does
            earcons_.play(Earcon::Error);
            say(pick.title + " has no tracks.");
            return;
        }
        // The Options screen goes away only once the download starts; a refusal is spoken on it.
        startDownload(pick.link, pick.title, pick.artist, tracks, sourceOf(pick), [this] {
            popScreen();
            earcons_.play(Earcon::Close);
        });
        return;
    }
    Selection selection;
    switch (pick.kind) {
    case Pick::Kind::Album:
        selection = remoteSelection(pick.title, tracks, RemoteSource{RemoteSource::Kind::Album, pick.id}, pick.artist, true, pick.id, pick.link);
        break;
    case Pick::Kind::Playlist:
        selection = remoteSelection(pick.title, tracks, RemoteSource{RemoteSource::Kind::Playlist, pick.id}, "", false, {}, pick.link);
        break;
    default: // an artist: no list on the server to resume
        selection = remoteSelection(pick.title, tracks, std::nullopt, pick.title, false, {}, pick.link);
        break;
    }
    selection.playedId = pick.playedId;
    tts_.stop(); // "Loading." is over
    perform(std::move(selection), done.action);
}

void App::failLoading(MusicServer::Error error) {
    loading_ = Loading{};
    tts_.stop();
    earcons_.play(Earcon::Error);
    say(errorText(error) + ".");
}

void App::cancelLoading() {
    if (loading_.request) serverAt(loading_.pick.link).cancel(loading_.request);
    loading_ = Loading{};
}

// The action finishes a task: the Options screen goes away by itself and the device says what happened.
void App::perform(Selection selection, QueueAction action) {
    resolveCachedPaths(selection.entries);
    if (selection.entries.empty()) { // an album with no tracks: stay on the Options
        earcons_.play(Earcon::Error);
        say(selection.title + " has no tracks.");
        return;
    }
    popScreen();
    earcons_.play(Earcon::Close);
    if (action == QueueAction::PlayNow) playNow(std::move(selection));
    else addToQueue(std::move(selection), action == QueueAction::PlayNext);
}

// Replaces the queue and plays. A server track opens in the background ("Loading."), and "Playing" is
// said once it has; leaving the list meanwhile drops the open.
void App::playNow(Selection selection) {
    playEntries(selection.list, std::move(selection.entries), 0,
                [this, title = std::move(selection.title), playedId = std::move(selection.playedId)] {
                    if (!playedId.empty()) markPlayed(playedId);
                    say("Playing " + title + ".");
                });
}

// After the current entry (`next`) or at the end. With no music queue loaded the entries become one that
// waits at its first entry, closed like a queue that ended: Play opens it.
void App::addToQueue(Selection selection, bool next) {
    const size_t tracks = selection.entries.size();
    const std::string added = "Added " + selectionText(selection.title, tracks) + ". ";
    auto put = [&](Queue& queue) {
        if (next) queue.playNext(selection.entries);
        else queue.append(selection.entries, random_);
        return queue.size();
    };
    if (book_ && music_) {
        say(added + std::to_string(put(queue_)) + " in queue.");
    } else if (parked_ && parked_->music) { // the book player has the player: the queue waits for Continue
        const size_t total = put(parked_->queue);
        say(added + (book_ ? std::to_string(total) + " in queue." : "Press play."));
    } else {
        Queue queue;
        queue.replace(std::move(selection.entries), 0);
        selection.list.source.reset(); // nothing has played: nothing to resume after a restart
        if (book_) { // a book is loaded: it keeps playing
            parked_ = Parked{std::move(selection.list), true, 0, 0.0, std::move(queue)};
            say(added + std::to_string(tracks) + " in queue.");
        } else {
            book_ = std::move(selection.list);
            music_ = true;
            queue_ = std::move(queue);
            fileIndex_ = 0;
            reopenAt_ = 0.0; // closed: Play opens the first entry
            say(added + "Press play.");
        }
    }
}

// Some entry of the music queue (loaded or parked) is a track of the downloaded list `source`: deleting
// it would take the file from under the queue.
bool App::queuedFrom(const RemoteSource& source, size_t link) const {
    const Queue* queue = book_ && music_ ? &queue_ : parked_ && parked_->music ? &parked_->queue : nullptr;
    if (!queue || queue->empty()) return false;
    if (link >= servers_.size()) return false;
    std::set<std::string> ids; // (a cache that is not enabled loads nothing)
    for (const CachedTrack& t : trackCacheOf(link).load(source).tracks) ids.insert(t.id);
    const ServerConfig& config = servers_[link].config; // by url and user: the server need not be reachable for its files to play
    for (const QueueEntry& e : queue->originalEntries())
        if (e.kind == QueueEntry::Kind::Remote && e.serverUrl == config.url && e.user == config.user && ids.count(e.trackId)) return true;
    return false;
}

// ---------------------------------------------------------- downloaded lists

namespace {
// What a downloaded list is called in a menu; a list saved without a name is still spoken as what it is.
std::string downloadedName(const CachedList& list) {
    if (!list.name.empty()) return list.name;
    return list.source.kind == RemoteSource::Kind::Album ? "Album" : "Playlist";
}

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

bool App::hasDownloads(size_t link) const {
    const TrackCache& store = trackCacheOf(link);
    if (!store.enabled()) return false;
    const auto lists = store.lists(true);
    return std::any_of(lists.begin(), lists.end(), [](const CachedList& list) {
        return list.source.kind != RemoteSource::Kind::KeptPlaylist;
    });
}

// Streaming > Downloaded: the lists whose manifests are in the cache, by name.
// Read from disk each time the menu is built, so it shows what changed meanwhile.
std::vector<MenuItem> App::downloadedMenu(size_t link) {
    std::vector<MenuItem> items;
    const TrackCache& store = trackCacheOf(link);
    const bool books = serverAt(link).catalogue() == MusicServer::Catalogue::Books;
    for (const CachedList& list : store.lists(true)) { // what the user downloaded, not the automatic cache
        if (list.source.kind == RemoteSource::Kind::KeptPlaylist) continue;
        MenuItem item;
        if (books && list.source.kind == RemoteSource::Kind::Album) {
            // A book that is whole plays from its files; one cut short opens as on the server (its files and chapters
            // are asked for: "Loading", "Server not reachable", Try again), where Download book goes on with it.
            const CachedList full = store.load(list.source); // lists() leaves out the files that are not whole
            if (isWholeDownload(full, completeTracks(store, list.source, full))) {
                item = submenu(downloadedName(list), [this, source = list.source, link] { return downloadedBookMenu(source, link); });
            } else {
                RemoteAlbum album;
                album.id = list.source.id;
                album.name = downloadedName(list);
                album.artist = list.artist;
                item = action(album.name, [this, album, link] { openBookPage(album, link); });
                item.okHint = "open";
            }
        } else {
            item = submenu(downloadedName(list), [this, source = list.source, link] { return downloadedListMenu(source, link); });
        }
        std::string about = list.source.kind == RemoteSource::Kind::Playlist ? "playlist" : list.artist;
        if (!about.empty()) item.value = [about] { return about; };
        items.push_back(std::move(item));
    }
    if (items.empty())
        items.push_back(action("No downloads", [this, books] {
            say(books ? "Nothing is downloaded. Choose Download book in a book." : "Nothing is downloaded. Choose Download album in an album.");
        }));
    return items;
}

// The tracks of a downloaded list that are whole on disk: "Play album" first, then
// "Delete download", then the tracks (trackItems(), with Delete in place of Download). They play from their files like any list.
std::vector<MenuItem> App::downloadedListMenu(const RemoteSource& source, size_t link) {
    const TrackCache& store = trackCacheOf(link);
    CachedList list = store.load(source);
    std::vector<RemoteTrack> tracks = completeTracks(store, source, list);
    if (tracks.empty()) return {action("No tracks", [this] { say("The tracks of this download are gone from the disk."); })};
    std::string title = downloadedName(list);
    return trackItems(title, tracks, source.kind == RemoteSource::Kind::Album ? "Play album" : "Play playlist", source,
                      list.artist, deleteDownloadItem(source, title, link), {}, link);
}

// A dangerous action: behind a list that opens on Cancel.
MenuItem App::deleteDownloadItem(const RemoteSource& source, const std::string& title, size_t link) {
    return submenu("Delete download", [this, source, title, link] {
        return std::vector<MenuItem>{
            action("Cancel", [this] { back(); }),
            action("Yes, delete", [this, source, title, link] { deleteDownload(source, title, link); }),
        };
    });
}

// A downloaded book of a Books server, from its files and the chapters its manifest kept: the book's page as the
// server gives it, with no question to the server.
std::vector<MenuItem> App::downloadedBookMenu(const RemoteSource& source, size_t link) {
    const TrackCache& store = trackCacheOf(link);
    const CachedList list = store.load(source);
    RemoteBook remote{completeTracks(store, source, list), list.chapters};
    if (remote.tracks.empty()) return {action("No tracks", [this] { say("The files of this download are gone from the disk."); })};
    RemoteAlbum album;
    album.id = source.id;
    album.name = downloadedName(list);
    album.artist = list.artist;
    return bookPage(album, remote, link);
}

// ---------------------------------------------------------- Music > Playlists

// The playlists of the servers set up now (the index holds no other server's, dropUnsetServers()): those in the index, and the downloaded ones from the cache
// manifests (a downloaded playlist deleted on the server, or never synced, is still there). One entry
// for a playlist of a server in both, named and counted as the index has it. Sorted by name.
std::vector<App::PlaylistEntry> App::playlistEntries() const {
    std::vector<PlaylistEntry> entries;
    std::set<std::pair<size_t, std::string>> seen;
    for (size_t link = 0; link < servers_.size(); ++link) {
        for (const auto& [ref, p] : index_.playlists()) {
            if (p.server != servers_[link].key) continue;
            seen.emplace(link, p.id);
            entries.push_back({link, p.id, p.name, size_t(std::max(p.songCount, 0))});
        }
        const TrackCache& store = trackCacheOf(link);
        if (!store.enabled()) continue;
        for (const CachedList& list : store.lists(true)) {
            if (list.source.kind != RemoteSource::Kind::Playlist || seen.contains({link, list.source.id})) continue;
            entries.push_back({link, list.source.id, downloadedName(list), list.tracks.size()});
        }
    }
    if (localPlaylists_) {
        for (const auto& p : localPlaylists_->list())
            entries.push_back({0, p.id, p.name, p.tracks.size(), true, p.tracks});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const PlaylistEntry& a, const PlaylistEntry& b) { return naturalLess(a.name, b.name); });
    return entries;
}

bool App::hasPlaylists() const {
    return !playlistEntries().empty();
}

// Music > Playlists is a local menu: read from the index and the manifests when it is opened.
std::vector<MenuItem> App::playlistsMenu() {
    std::vector<MenuItem> items;
    if (localPlaylists_) items.push_back(submenu("Create playlist", [this] {
        std::vector<MenuItem> destinations{action("On the device", [this] { createPlaylist(0, true); })};
        for (size_t link = 0; link < servers_.size(); ++link)
            if (serverAt(link).configured() && serverAt(link).supportsPlaylistEditing())
                destinations.push_back(action(servers_[link].name, [this, link] { createPlaylist(link, false); }));
        return destinations;
    }));
    for (const PlaylistEntry& p : playlistEntries()) {
        MenuItem item = action(p.name, [this, p] {
            if (p.local) openMenu(p.name, [this, id = p.id] { return localPlaylistMenu(id); });
            else openPlaylist(p.link, p.id, p.name);
        });
        item.okHint = "open";
        if (p.tracks > 0) item.value = [n = p.tracks] { return countValue(n, "track"); };
        if (p.local) {
            if (!p.localTracks.empty()) {
                Book book; book.title = p.name; book.files = p.localTracks;
                item = withOptions(std::move(item), localAlbumPick(book, ""));
            }
            items.push_back(std::move(item));
        } else items.push_back(withOptions(std::move(item), remotePick(Pick::Kind::Playlist, p.id, p.name, "", {}, p.link)));
    }
    if (items.empty())
        items.push_back(action("No playlists", [this] { say("Make a playlist on the server, or save the queue as a playlist."); }));
    return items;
}

// A playlist that is whole on the disk opens from its files at once, with no question to the server;
// any other loads its tracks (Lists from the server: "Loading", the error, "Try again", "Downloaded").
void App::openPlaylist(size_t link, const std::string& id, const std::string& name) {
    RemoteSource source{RemoteSource::Kind::Playlist, id};
    const TrackCache& store = trackCacheOf(link);
    if (state_.getInt(keptPlaylistKey(link, id), 0) &&
        state_.getInt(keptReadyKey(link, id), -1) == state_.getInt(keptRevisionKey(link, id), 0)) {
        RemoteSource kept{RemoteSource::Kind::KeptPlaylist, id};
        CachedList cached = store.load(kept);
        auto complete = completeTracks(store, kept, cached);
        if (cached.source.kind == RemoteSource::Kind::KeptPlaylist && cached.source.id == id && cached.order.empty()) {
            openMenu(name, [this, name, source, link] { return trackItems(name, {}, "Play playlist", source, "", std::nullopt, {}, link); });
            return;
        }
        if (!cached.order.empty()) {
            std::vector<RemoteTrack> ordered;
            for (const auto& trackId : cached.order) {
                auto found = std::find_if(complete.begin(), complete.end(), [&](const RemoteTrack& t) { return t.id == trackId; });
                auto entry = std::find_if(cached.tracks.begin(), cached.tracks.end(), [&](const CachedTrack& t) { return t.id == trackId; });
                if (found == complete.end() || entry == cached.tracks.end() || !entry->pinned || entry->changed == kPartialMark) { ordered.clear(); break; }
                ordered.push_back(*found);
            }
            if (!ordered.empty()) {
                openMenu(name, [this, name, source, link, ordered] { return trackItems(name, ordered, "Play playlist", source, "", std::nullopt, {}, link); });
                return;
            }
        }
    }
    if (store.enabled() && !state_.getInt(stalePlaylistKey({link, id, name, 0, false}), 0)) {
        CachedList cached = store.load(source);
        if (isWholeDownload(cached, completeTracks(store, source, cached))) {
            openMenu(name, [this, source, link] { return downloadedListMenu(source, link); });
            return;
        }
    }
    openRemoteList(link, name,
                   fetchPages<RemoteTrack>(
                       [this, link, id](int, MusicServer::Done<RemoteTrack> done) { return serverAt(link).playlistTracks(id, std::move(done)); },
                       [this, name, id, link](const std::vector<RemoteTrack>& tracks) {
                           return trackItems(name, tracks, "Play playlist", {RemoteSource::Kind::Playlist, id}, "", std::nullopt, {}, link);
                       }),
                   0, "No tracks", "This playlist is empty. Add tracks to it on the server.");
}

// Removes the list's folder. The list that is loaded in the player is refused (its
// next tracks would have to come from the network); a download of it is cancelled first.
void App::deleteDownload(const RemoteSource& source, const std::string& title, size_t link) {
    // A server book (not the music queue, whose lists queuedFrom() finds by server).
    const bool loaded = !music_ && book_ && book_->remote && book_->source && book_->link == link && sameList(*book_->source, source);
    if (loaded || queuedFrom(source, link)) {
        earcons_.play(Earcon::Error);
        say("Cannot delete " + title + " while it is loaded in the player. Play something else first.");
        return;
    }
    if (downloading(source, link)) {
        servers_[link].downloader.cancel(download_->request);
        download_.reset();
    }
    dropAutoCache(source, link);
    if (!trackCacheOf(link).remove(source)) {
        earcons_.play(Earcon::Error);
        say("Cannot delete " + title + ".");
        return;
    }
    linkState(link).cacheIndex.reset(); // its files are gone: the index is made again from the manifests left
    popScreen();         // the confirmation, then the list that is gone, back to Downloaded (rebuilt)
    popScreen();
    earcons_.play(Earcon::Close);
    say("Deleted " + title + ". " + itemSpeech(screens_.back()));
}

// Says "Checking" now and the answer when it comes, if the user is still on
// this screen. Leaving the screen cancels it.
void App::checkServer(size_t link) {
    if (ping_) serverAt(pingLink_).cancel(std::exchange(ping_, 0));
    std::uint64_t screenId = screens_.back().id;
    pingScreen_ = screenId;
    pingLink_ = link;
    say("Checking the server.");
    ping_ = serverAt(link).ping([this, screenId, link](MusicServer::Error error, MusicServer::ServerInfo info) {
        ping_ = 0;
        if (sleeping_ || screens_.empty() || screens_.back().id != screenId) return; // the user has moved on
        tts_.stop();
        if (error != MusicServer::Error::None) {
            earcons_.play(Earcon::Error);
            say(errorText(error) + ".");
            return;
        }
        clearServerDown(link); // it answers
        say(connectedText(info));
    });
}

std::optional<App::StreamPlace> App::savedStream() const {
    const std::string saved = serverIdentity(state_.getString(kLastStreamServer)), user = state_.getString(kLastStreamUser);
    StreamPlace place;
    bool found = false;
    for (size_t i = 0; i < servers_.size() && !found; ++i)
        if (servers_[i].server.configured() && !servers_[i].config.url.empty() && saved == servers_[i].config.url &&
            user == servers_[i].config.user)
            place.link = i, found = true;
    if (!found) return std::nullopt; // another server, or none: its ids mean nothing here
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
    playRemote(place.title, tracks, index, place.source, position, {}, place.link);
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
    const TrackCache& store = trackCacheOf(place->link);
    CachedList cached = store.enabled() ? store.load(place->source) : CachedList{};
    std::vector<RemoteTrack> onDisk = completeTracks(store, place->source, cached);
    bool wholeDownload = isWholeDownload(cached, onDisk);
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
    resumeLink_ = place->link;
    resume_ = place->source.kind == RemoteSource::Kind::Album ? serverAt(place->link).albumTracks(place->source.id, done)
                                                               : serverAt(place->link).playlistTracks(place->source.id, done);
}

void App::cancelResume() {
    if (resume_) serverAt(resumeLink_).cancel(std::exchange(resume_, 0));
    resumeScreen_ = 0;
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
    items.push_back(choice("Rewind on resume", kRewindOnResume, seconds({0, 2, 5, 10}), 2));
    items.push_back(choice("Long press time", kLongPress, range(300, 1500, 100, " milliseconds"), 600,
                           [this](int v) { gestures_.setLongPress(std::chrono::milliseconds(v)); }));
    items.push_back(choice("Menu wrap around", kMenuWrap, {{0, "off"}, {1, "on"}}, 1));
    items.push_back(choice("Auto sleep", kAutoSleep,
                           {{1, "1 minute"}, {2, "2 minutes"}, {5, "5 minutes"}, {10, "10 minutes"}, {30, "30 minutes"}, {0, "never"}}, 5));
    // Read when a track is opened: a change takes effect with the next one.
    items.push_back(choice("Streaming quality", kStreamingBitrate, kStreamingQualities, kOriginalQuality));
    items.push_back(choice("Download quality", kDownloadBitrate, kStreamingQualities, kOriginalQuality));
    // Read by the automatic cache when it queues a track; a lower limit evicts on the next exit or cached track.
    items.push_back(choice("Cache size", kCacheLimit,
                           {{0, "off"}, {1, "1 gigabyte"}, {4, "4 gigabytes"}, {16, "16 gigabytes"}},
                           kDefaultCacheGigabytes));
    items.push_back(choice("Replace cached copies", kCacheUpgrade, {{0, "off"}, {1, "on"}}, 0));
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
        say("This player has no screen. Left and Right move one item through a menu. Press deeper to jump ten items, "
            "or hold to reach the first or last item. OK opens the item; hold OK for options. Esc goes back, "
            "and holding Esc goes to Home. Short Esc at Home returns to the current player without resuming it. "
            "In a player, Left and Right change chapter or track. Hold to seek; press deeper to seek faster, "
            "and release to stop. Up, Down and Talk are unused. Play pause starts and stops playback, "
            "and holding it tells you where you are. The volume keys change the volume everywhere. "
            "Power puts the player to sleep.");
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
        say("Key describer on. Press any key to hear what it does. Hold Esc to leave.");
    }));
    return items;
}

std::vector<MenuItem> App::playerMenu() { return music_ ? musicPlayerMenu() : bookPlayerMenu(); }

// What both Player menus have in common.
MenuItem App::sleepTimerItem() {
    MenuItem timer = submenu("Sleep timer", [this] { return sleepTimerMenu(); });
    timer.selected = [this] { return sleepTimerIndex(); };
    return timer;
}

MenuItem App::goToStartItem(const std::string& label) {
    return action(label, [this] {
        // Once it is open; if it fails, the error is said and the user stays in the menu.
        loadFile(0, 0, willPlay(), Origin::Menu, [this, screen = screens_.back().id] { enterPlayerFrom(screen); });
    });
}

void App::sayPlayerInfo() {
    if (!book_) return;
    auto ch = chapters();
    say(book_->title + ". " + std::to_string(ch.size()) + " " + unit() + "s. " + playerStatus());
}

// The music player (docs/queue.md, "Player menu (music)").
std::vector<MenuItem> App::musicPlayerMenu() {
    MenuItem queue = submenu("Queue", [this] { return queueMenu(); });
    queue.selected = [this] { return fileIndex_; }; // the play order position of the current entry
    MenuItem add = submenu("Add to playlist", [this] { return addToPlaylistMenu(); });
    add.selected = [this] {
        auto list = playlistEntries();
        if (queue_.empty()) return size_t(0);
        size_t n = 0;
        for (const auto& p : list) {
            if (!playlistTrackId(p, queue_.at(fileIndex_))) continue;
            std::string key = (p.local ? "local:" : servers_[p.link].key + ":") + TrackCache::safeName(p.id);
            if (key == lastPlaylist_) return n;
            ++n;
        }
        return size_t(0);
    };
    std::vector<MenuItem> items{std::move(queue), shuffleItem(), repeatItem(), goToStartItem("Go to first track"), sleepTimerItem()};
    if (localPlaylists_) items.push_back(std::move(add));
    return items;
}

// A "Setting with list" item that the queue backs instead of config.yaml: OK opens the list on the current
// value, choosing one applies it at once and goes back.
MenuItem App::queueSetting(const std::string& label, std::vector<std::string> names, std::function<size_t()> current,
                           std::function<bool(size_t)> apply) {
    MenuItem item;
    item.label = label;
    item.value = [names, current] { return names[current()]; };
    item.selected = current;
    item.submenu = [this, names, current, apply] {
        std::vector<MenuItem> items;
        const size_t now = current();
        for (size_t i = 0; i < names.size(); ++i) {
            MenuItem option = action(names[i], [this, apply, i] {
                if (apply(i)) back(); // refused: the list stays, with the reason said
            });
            if (i == now) option.value = [] { return std::string("current"); };
            items.push_back(std::move(option));
        }
        return items;
    };
    return item;
}

// The lists are in the order of the enums.
MenuItem App::shuffleItem() {
    return queueSetting("Shuffle", {"off", "tracks", "albums"}, [this] { return size_t(queue_.shuffle()); },
                        [this](size_t i) { return setShuffle(QueueShuffle(i)); });
}

MenuItem App::repeatItem() {
    // Read when a track ends, so there is nothing to apply now.
    return queueSetting("Repeat", {"off", "all", "album", "track"}, [this] { return size_t(queue_.repeat()); },
                        [this](size_t i) {
                            queue_.setRepeat(QueueRepeat(i));
                            return true;
                        });
}

// The current entry stays current and keeps playing; what follows is shuffled (or put back in order).
// Not while a track the user chose is opening: its undo names the track by its place in the play order.
bool App::setShuffle(QueueShuffle mode) {
    if (queueLocked()) return false;
    queue_.setShuffle(mode, random_);
    followQueue();
    return true;
}

// The queue was reordered: what the player holds by the position of its track follows the current entry.
// Shuffle off, Move and Remove all come here.
void App::followQueue() {
    fileIndex_ = queue_.position();
    if (sleepAtChapterEnd_ >= 0) sleepAtChapterEnd_ = filePerChapter() ? int(fileIndex_) : currentChapter();
    if (pendingOpen_) pendingOpen_->file = fileIndex_; // the track that is opening is the current one
}

std::vector<MenuItem> App::bookPlayerMenu() {
    MenuItem chapters = submenu("Chapters", [this] { return chaptersMenu(); });
    chapters.selected = [this] { return size_t(currentChapter()); };
    std::vector<MenuItem> items = {std::move(chapters), sleepTimerItem(), action("Book info", [this] { sayPlayerInfo(); }),
                                   goToStartItem("Go to start of book")};
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

// ---- the Queue list

std::vector<MenuItem> App::queueMenu() {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < queue_.size(); ++i) {
        // Once it is open; if it fails, the error is said and the user stays in the list.
        MenuItem entry = action(trackTitle(i), [this, i] {
            gotoChapter(int(i), Origin::Menu, [this, screen = screens_.back().id] { enterPlayerFrom(screen); });
        });
        if (i == fileIndex_) entry.value = [] { return std::string("current"); };
        entry.okHint = "play";
        entry.options = [this, i] { return queueEntryOptions(i); };
        items.push_back(std::move(entry));
    }
    items.push_back(action("Save as playlist", [this] { saveAsPlaylist(); }));
    if (previous_) {
        MenuItem previous = action("Previous queue", [this] { restorePrevious(); });
        previous.value = [n = previous_->queue.size()] { return countValue(n, "track"); };
        items.push_back(std::move(previous));
    }
    // A dangerous action: behind a list that opens on Cancel.
    items.push_back(submenu("Clear queue", [this] {
        return std::vector<MenuItem>{
            action("Cancel", [this] { back(); }),
            action("Clear queue", [this] { clearQueue("Queue cleared."); }),
        };
    }));
    return items;
}

namespace {
// "Queue, 27 October 14:05": English month names whatever the locale, the day without a leading zero.
std::string queuePlaylistName(std::time_t when) {
    static const char* const months[] = {"January", "February", "March",     "April",   "May",      "June",
                                         "July",    "August",   "September", "October", "November", "December"};
    const std::tm t = *std::localtime(&when); // as the spoken time does
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", t.tm_hour, t.tm_min);
    return "Queue, " + std::to_string(t.tm_mday) + " " + months[t.tm_mon % 12] + " " + buf;
}
} // namespace

// All playlist mutations belong to the initiating screen. Leaving it cancels the callback.
void App::cancelPlaylistEdit() {
    if (playlistEdit_.request) serverAt(playlistEdit_.link).cancel(playlistEdit_.request);
    playlistEdit_ = {};
}

void App::markPlaylistScreenDone(std::uint64_t id, const std::string& message) {
    Screen* screen = findScreen(id);
    if (!screen) return;
    screen->build = {};
    screen->items = {action(message, [this] { back(); })};
    screen->index = 0;
}

void App::playlistEditDone(MusicServer::Error error, std::function<void(bool)> success) {
    const auto edit = std::exchange(playlistEdit_, PlaylistEdit{});
    if (!findScreen(edit.screen) || sleeping_ || shutDown_) return;
    if (error != MusicServer::Error::None) {
        if (screens_.back().id == edit.screen) {
            earcons_.play(Earcon::Error);
            say(errorText(error) + ".");
        }
        return;
    }
    success(screens_.back().id == edit.screen);
}

void App::createPlaylist(size_t link, bool local) {
    if (playlistEdit_.request) return say("Saving.");
    std::string name = queuePlaylistName(clock_.wallTime());
    name.replace(0, 5, "Playlist");
    if (local) {
        if (!localPlaylists_ || !localPlaylists_->create(name)) {
            earcons_.play(Earcon::Error); return say("Could not save playlist.");
        }
        popScreen();
        return say("Created " + name + ".");
    }
    const auto screen = screens_.back().id;
    playlistEdit_ = {screen, link, 0};
    say("Saving.");
    playlistEdit_.request = serverAt(link).createPlaylist(name, {}, [this, link, name, screen](MusicServer::Error error, std::string id) {
        playlistEditDone(error, [this, link, name, id, screen](bool visible) {
            if (!id.empty()) {
                IndexPlaylist playlist{servers_[link].key, id, name, 0, clock_.wallTime()};
                index_.addPlaylist(playlist);
                index_.savePlaylists();
                if (catalogueRequest_ && catalogue_.key == servers_[link].key) catalogue_.savedPlaylists.push_back(playlist);
            }
            if (visible) { popScreen(); say("Created " + name + "."); }
            else markPlaylistScreenDone(screen, "Playlist created");
        });
    });
}

std::optional<std::string> App::playlistTrackId(const PlaylistEntry& playlist, const QueueEntry& track) const {
    if (playlist.local) {
        if (track.kind != QueueEntry::Kind::Local || track.path.empty()) return std::nullopt;
        return track.path;
    }
    if (!servers_[playlist.link].server.supportsPlaylistEditing()) return std::nullopt;
    if (track.kind == QueueEntry::Kind::Remote) {
        if (linkOf(track) != playlist.link || track.trackId.empty()) return std::nullopt;
        return track.trackId;
    }
    auto hit = index_.findByLocation(track.path);
    if (!hit.track) return std::nullopt;
    const std::string prefix = LibraryIndex::seenRef(servers_[playlist.link].key, SeenKind::Track, "");
    for (const auto& ref : hit.track->seenOn)
        if (ref.starts_with(prefix) && ref.size() > prefix.size()) return ref.substr(prefix.size());
    return std::nullopt;
}

std::vector<MenuItem> App::addToPlaylistMenu() {
    std::vector<MenuItem> result;
    if (!queue_.empty()) for (const auto& p : playlistEntries()) {
        if (!playlistTrackId(p, queue_.at(fileIndex_))) continue;
        result.push_back(action(p.name, [this, p] { addCurrentToPlaylist(p); }));
    }
    if (result.empty()) result.push_back(action("No eligible playlists", [this] { say("Not available."); }));
    return result;
}

void App::addCurrentToPlaylist(const PlaylistEntry& p) {
    if (queue_.empty()) return say("Not available.");
    auto id = playlistTrackId(p, queue_.at(fileIndex_));
    if (!id) return say("Not available.");
    const auto screen = screens_.back().id;
    auto finish = [this, p, screen](bool visible) {
        lastPlaylist_ = (p.local ? "local:" : servers_[p.link].key + ":") + TrackCache::safeName(p.id);
        state_.setString("last_playlist", lastPlaylist_);
        if (!p.local) { invalidatePlaylistSnapshot(p); changePlaylistCount(p, 1); }
        if (visible) { popScreen(); say("Added to playlist."); }
        else markPlaylistScreenDone(screen, "Added to playlist");
    };
    if (p.local) {
        if (!localPlaylists_->add(p.id, *id)) { earcons_.play(Earcon::Error); return say("Could not save playlist."); }
        return finish(true);
    }
    if (playlistEdit_.request) return say("Saving.");
    playlistEdit_ = {screens_.back().id, p.link, 0};
    say("Saving.");
    playlistEdit_.request = serverAt(p.link).addPlaylistTrack(p.id, *id, [this, finish](MusicServer::Error error) {
        playlistEditDone(error, finish);
    });
}

std::vector<MenuItem> App::localPlaylistMenu(const std::string& id) {
    if (!localPlaylists_) return {};
    auto lists = localPlaylists_->list();
    auto found = std::find_if(lists.begin(), lists.end(), [&](const LocalPlaylist& p) { return p.id == id; });
    if (found == lists.end()) return {action("No playlist", [this] { say("Not available."); })};
    PlaylistEntry p{0, found->id, found->name, found->tracks.size(), true};
    Book book; book.title = p.name; book.files = found->tracks;
    std::vector<MenuItem> items;
    items.push_back(action("Play playlist", [this, book] {
        if (book.files.empty()) return say("No tracks.");
        if (playMusic(book, 0)) enterPlayer();
    }));
    for (size_t i = 0; i < book.files.size(); ++i) {
        MenuItem track = action(partTitle(book, i), [this, book, i] { if (playMusic(book, i)) enterPlayer(); });
        track.okHint = "play";
        track = withOptions(std::move(track), localTrackPick(book, i, ""));
        auto options = track.options;
        track.options = [this, options, p, i] {
            auto result = options();
            result.push_back(action("Remove from playlist", [this, p, i] { removeFromPlaylist(p, i); }));
            return result;
        };
        items.push_back(std::move(track));
    }
    items.push_back(deletePlaylistItem(p));
    return items;
}

void App::changePlaylistCount(const PlaylistEntry& p, int delta) {
    auto key = LibraryIndex::seenRef(servers_[p.link].key, SeenKind::Playlist, p.id);
    auto found = index_.playlists().find(key);
    if (found == index_.playlists().end()) return;
    auto playlist = found->second;
    const auto count = static_cast<long long>(playlist.songCount) + delta;
    playlist.songCount = static_cast<int>(std::clamp(count, 0LL, static_cast<long long>(std::numeric_limits<int>::max())));
    if (syncRunning_ && catalogue_.key == playlist.server) catalogue_.editedPlaylists[playlist.id] = playlist;
    index_.addPlaylist(std::move(playlist));
    index_.savePlaylists();
}

std::string App::stalePlaylistKey(const PlaylistEntry& p) const {
    return "playlist.stale:" + TrackCache::safeName(servers_[p.link].key) + ":" + TrackCache::safeName(p.id);
}

void App::invalidatePlaylistSnapshot(const PlaylistEntry& p) {
    const auto key = stalePlaylistKey(p);
    const int old = state_.getInt(key, 0);
    state_.setInt(key, old == std::numeric_limits<int>::max() ? old : old + 1);
    const auto revision = keptRevisionKey(p.link, p.id);
    const int current = state_.getInt(revision, 0);
    state_.setInt(revision, current == std::numeric_limits<int>::max() ? current : current + 1);
    if (state_.getInt(keptPlaylistKey(p.link, p.id), 0)) scheduleKeptPlaylist(p.link, p.id, p.name);
}

// A download stores each track only once, so its rows are not server positions. Always
// ask for the current ordered list and let the user select an occurrence to remove.
void App::selectServerPlaylistRemoval(const PlaylistEntry& p) {
    openRemoteList(p.link, "Remove from playlist",
        fetchPages<RemoteTrack>(
            [this, p](int, MusicServer::Done<RemoteTrack> done) {
                return serverAt(p.link).playlistTracks(p.id, std::move(done));
            },
            [this, p](const std::vector<RemoteTrack>& tracks) {
                std::vector<MenuItem> items;
                for (size_t i = 0; i < tracks.size(); ++i) {
                    const auto title = tracks[i].title.empty() ? "Track " + std::to_string(i + 1) : tracks[i].title;
                    items.push_back(action(title, [this, p, i] { removeFromPlaylist(p, i); }));
                }
                return items;
            }), 0, "No tracks", "This playlist is empty.");
}

void App::removeFromPlaylist(const PlaylistEntry& p, size_t index) {
    if (p.local) {
        if (!localPlaylists_ || !localPlaylists_->removeTrack(p.id, index)) { earcons_.play(Earcon::Error); return say("Could not save playlist."); }
        popScreen();
        return say("Removed from playlist.");
    }
    if (playlistEdit_.request) return say("Saving.");
    const auto screen = screens_.back().id;
    // The selector is above the track's Options, which is above the playlist.
    const auto listScreen = screens_[screens_.size() - 3].id;
    playlistEdit_ = {screen, p.link, 0};
    say("Saving.");
    playlistEdit_.request = serverAt(p.link).removePlaylistTrack(p.id, index, [this, p, listScreen, screen](MusicServer::Error error) {
        playlistEditDone(error, [this, p, listScreen, screen](bool visible) {
            invalidatePlaylistSnapshot(p);
            changePlaylistCount(p, -1);
            if (visible) closeScreens(screens_.size() - 2);
            Screen* listView = findScreen(listScreen);
            if (listView) {
                listView->items.clear();
                listView->build = {};
                RemoteList& list = remoteLists_[listScreen];
                list = {};
                list.link = p.link;
                list.emptyLabel = "No tracks";
                list.emptyHelp = "This playlist is empty.";
                list.fetch = fetchPages<RemoteTrack>(
                    [this, p](int, MusicServer::Done<RemoteTrack> done) { return serverAt(p.link).playlistTracks(p.id, std::move(done)); },
                    [this, p](const std::vector<RemoteTrack>& tracks) {
                        return trackItems(p.name, tracks, "Play playlist", {RemoteSource::Kind::Playlist, p.id}, "", std::nullopt, {}, p.link);
                    });
                requestPage(listScreen, 0, true); showStatus(listScreen);
            }
            if (visible) say("Removed from playlist.");
            else markPlaylistScreenDone(screen, "Removed from playlist");
        });
    });
}

MenuItem App::deletePlaylistItem(const PlaylistEntry& p) {
    return submenu("Delete playlist", [this, p] {
        return std::vector<MenuItem>{
            action("Cancel", [this] { popScreen(); say("Cancelled."); }),
            action("Yes, delete", [this, p] { deletePlaylist(p); })};
    });
}

void App::deletePlaylist(const PlaylistEntry& p) {
    const auto screen = screens_.back().id;
    const auto listScreen = screens_[screens_.size() - 2].id;
    auto finish = [this, p, screen, listScreen](bool visible) {
        if (!p.local) {
            setPlaylistKept(p.link, p.id, p.name, false);
            std::vector<IndexPlaylist> keep;
            for (const auto& [key, item] : index_.playlists())
                if (item.server == servers_[p.link].key && item.id != p.id) keep.push_back(item);
            index_.replacePlaylists(servers_[p.link].key, keep);
            index_.savePlaylists();
            if (syncRunning_ && catalogue_.key == servers_[p.link].key) {
                catalogue_.deletedPlaylists.insert(p.id);
                catalogue_.editedPlaylists.erase(p.id);
            }
        }
        if (visible) { popScreen(); popScreen(); say("Playlist deleted."); }
        else {
            markPlaylistScreenDone(screen, "Playlist deleted");
            markPlaylistScreenDone(listScreen, "Playlist deleted");
        }
    };
    if (p.local) {
        if (!localPlaylists_ || !localPlaylists_->remove(p.id)) { earcons_.play(Earcon::Error); return say("Could not save playlist."); }
        return finish(true);
    }
    if (playlistEdit_.request) return say("Saving.");
    playlistEdit_ = {screens_.back().id, p.link, 0};
    say("Saving.");
    playlistEdit_.request = serverAt(p.link).deletePlaylist(p.id, [this, finish](MusicServer::Error error) { playlistEditDone(error, finish); });
}

// Saves the tracks of the queue's first server (the server of its first track from a server), in play order,
// as a playlist on it. The queue is not touched,
// whatever the answer is; the answer is spoken on the Queue list it was asked from, and dropped if the
// user has left it (closeScreens(), sleep, power off).
void App::saveAsPlaylist() {
    if (saving_.request) return say("Saving.");
    std::optional<size_t> target;
    for (size_t i = 0; i < queue_.size() && !target; ++i) target = linkOf(queue_.at(i));
    std::vector<std::string> ids;
    for (size_t i = 0; i < queue_.size(); ++i)
        if (target && linkOf(queue_.at(i)) == target) ids.push_back(queue_.at(i).trackId); // the other servers' tracks are "not saved"
    if (ids.empty()) {
        earcons_.play(Earcon::Error);
        return say("Only tracks from the server can be saved.");
    }
    ids.resize(std::min(ids.size(), MusicServer::kMaxPlaylistTracks)); // the rest would not fit in the request
    const std::uint64_t screen = screens_.back().id;
    saving_ = Saving{screen, 0, queuePlaylistName(clock_.wallTime()), queue_.size() - ids.size(), ids.size(), *target, servers_[*target].key};
    say("Saving.");
    saving_.request = serverAt(*target).createPlaylist(saving_.name, std::move(ids), [this](MusicServer::Error error, std::string playlistId) { // never runs once cancelSaving() has cancelled it
        const Saving done = std::exchange(saving_, Saving{});
        tts_.stop(); // "Saving." is over
        if (error != MusicServer::Error::None) {
            earcons_.play(Earcon::Error);
            return say(errorText(error) + ".");
        }
        if (!playlistId.empty() && !done.server.empty()) { // in the index at once, without a sync
            IndexPlaylist playlist{done.server, playlistId, done.name, int(done.songCount), clock_.wallTime()};
            index_.addPlaylist(playlist);
            index_.savePlaylists();
            if (catalogueRequest_ && done.server == catalogue_.key) catalogue_.savedPlaylists.push_back(std::move(playlist)); // see fetchPlaylists()
        }
        earcons_.play(Earcon::Close);
        say("Saved " + done.name + "." + (done.notSaved ? " " + countValue(done.notSaved, "track") + " not saved." : ""));
    });
}

void App::cancelSaving() {
    if (saving_.request) serverAt(saving_.link).cancel(saving_.request);
    saving_ = Saving{};
}

// `position` is in the play order. The Queue list under these Options is built from the queue as it is
// now and nothing changes the queue while they are open, so the position still names the same entry.
std::vector<MenuItem> App::queueEntryOptions(size_t position) {
    MenuItem move = submenu("Move", [this, position] { return queueMovePositions(position); });
    move.selected = [position] { return position; }; // opens on the entry's own place
    return {
        action("Play next", [this, position] { queuePlayNext(position); }),
        std::move(move),
        action("Remove", [this, position] { queueRemove(position); }),
    };
}

// Every place of the queue, spoken "3, Rain": a setting with a list, on the entry's own place.
std::vector<MenuItem> App::queueMovePositions(size_t from) {
    std::vector<MenuItem> items;
    for (size_t to = 0; to < queue_.size(); ++to) {
        MenuItem place = action(std::to_string(to + 1), [this, from, to] { queueMove(from, to); });
        place.value = [this, to] { return trackTitle(to); };
        items.push_back(std::move(place));
    }
    return items;
}

// An action on an entry finished its task: the `screensAbove` screens over the Queue list go away, the
// list is built again for the new queue and stands on `index`, and the device says what happened.
void App::finishQueueEdit(size_t screensAbove, size_t index, const std::string& said) {
    closeScreens(screens_.size() - (screensAbove - 1));
    popScreen(); // rebuilds the Queue list
    Screen& list = screens_.back();
    // On an entry: not on Save as playlist, which follows the last one, so OK again cannot start a request.
    const size_t last = queue_.empty() ? 0 : queue_.size() - 1;
    list.index = std::min({index, last, list.items.empty() ? 0 : list.items.size() - 1});
    earcons_.play(Earcon::Close);
    say(said);
}

// A track the user chose (a menu, Next / Previous) is still opening. If it fails, the track it replaced is made
// current again by its old place in the queue, so the order must not change under it: wait.
bool App::queueLocked() {
    if (!pendingOpen_ || (pendingOpen_->origin != Origin::Menu && pendingOpen_->origin != Origin::Key)) return false;
    earcons_.play(Earcon::Error);
    say("Loading. Try again when it has started.");
    return true;
}

void App::queuePlayNext(size_t position) {
    if (queueLocked()) return;
    const std::string title = trackTitle(position);
    const size_t current = queue_.position();
    if (position == current) { // nothing to do, so the Options stay
        earcons_.play(Earcon::Error);
        say(title + " is the current entry.");
        return;
    }
    // Right after the current entry, which has moved one place left when the entry came from before it.
    const size_t to = position < current ? current : current + 1;
    if (position != to) {
        queue_.move(position, to);
        followQueue();
    }
    finishQueueEdit(1, to, title + " will play next.");
}

void App::queueMove(size_t from, size_t to) {
    if (queueLocked()) return;
    if (from != to) {
        queue_.move(from, to);
        followQueue();
    }
    const std::string said = from == to ? "Not moved." : "Moved to " + std::to_string(to + 1) + " of " + std::to_string(queue_.size()) + ".";
    finishQueueEdit(2, to, said);
}

void App::queueRemove(size_t position) {
    if (queueLocked()) return;
    const std::string title = trackTitle(position);
    if (queue_.size() == 1) { // the last one: like Clear queue
        clearQueue("Removed " + title + ". The queue is empty.");
        return;
    }
    bool removedCurrent = false;
    queue_.remove(position, &removedCurrent);
    if (removedCurrent) { // stopped; the next entry is current and waits, closed, for Play
        stopPlayer();
        sleepAtChapterEnd_ = -1; // the end of the track it was set for
        reopenAt_ = 0.0;
    }
    followQueue();
    std::string said = "Removed " + title + ". ";
    said += removedCurrent ? trackTitle(queue_.position()) + " is current, paused." : std::to_string(queue_.size()) + " in queue.";
    finishQueueEdit(1, removedCurrent ? queue_.position() : position, said);
}

// The queue is kept as Previous queue when it was edited: it is the way back.
void App::clearQueue(const std::string& said) {
    if (queue_.edited()) previous_ = PreviousQueue{*book_, queue_, place()};
    unloadQueue(said);
}

// What playing the current entry leaves behind goes: its open (and the open's undo), its preload, its scrobble.
void App::stopPlayer() {
    pendingOpen_.reset();
    cancelMarkPing();
    player_.pause();
    player_.close();
    if (readAhead_) {
        player_.cancelPreload();
        readAhead_.reset();
    }
    scrobble_.reset();
    dry_.reset();
}

// Nothing is loaded in the music player any more: it stops, and Home no longer offers it (what a restart
// would resume is forgotten too). The other player is not touched. Said on Home, where the user lands.
void App::unloadQueue(const std::string& said) {
    stopPlayer(); // without the undo of an open, which would bring the queue back
    reopenAt_.reset();
    restoredQueue_ = false;
    sleepAtChapterEnd_ = -1;
    queue_ = Queue{};
    book_.reset();
    music_ = false;
    fileIndex_ = 0;
    state_.clearPrefix(kLastStreamPrefix);
    if (screens_.size() > 1) {
        closeScreens(2);
        popScreen(); // Home is built again
    }
    earcons_.play(Earcon::Close);
    say(said + " " + itemSpeech(screens_.back()));
}

// ---------------------------------------------------------------- playback

// ---- the tracks of the loaded player (the queue for music, the files of a book)

size_t App::trackCount() const {
    if (music_) return queue_.size();
    return book_ ? book_->files.size() : 0;
}

bool App::trackRemote(size_t i) const {
    if (i >= trackCount()) return false;
    return music_ ? queue_.at(i).kind == QueueEntry::Kind::Remote : book_->remote;
}

// Where a remote entry plays from: the server of that url and user, if it is set up now.
std::optional<size_t> App::linkOf(const QueueEntry& e) const {
    if (e.kind != QueueEntry::Kind::Remote) return std::nullopt;
    for (size_t i = 0; i < servers_.size(); ++i)
        if (servers_[i].server.configured() && servers_[i].config.url == e.serverUrl && servers_[i].config.user == e.user) return i;
    return std::nullopt;
}

std::optional<size_t> App::trackLink(size_t i) const {
    if (!trackRemote(i)) return std::nullopt;
    if (music_) return linkOf(queue_.at(i));
    return book_->link < servers_.size() ? std::optional<size_t>(book_->link) : std::nullopt; // a server book
}

std::string App::trackKey(size_t i) const {
    if (music_) return queue_.at(i).kind == QueueEntry::Kind::Remote ? queue_.at(i).trackId : queue_.at(i).path;
    return book_->files[i].string();
}

// The spoken name of a track. A remote one is never named after its file, which is the server's track id.
std::string App::trackTitle(size_t i) const {
    if (!music_) return partTitle(*book_, i);
    const QueueEntry& e = queue_.at(i);
    if (!e.title.empty()) return e.title;
    return e.kind == QueueEntry::Kind::Remote ? "Track " + std::to_string(i + 1) : chapterTitle(e.path);
}

double App::trackDuration(size_t i) const {
    if (music_) return i < queue_.size() ? queue_.at(i).duration : 0;
    return book_ && i < book_->durations.size() ? book_->durations[i] : 0;
}

std::optional<size_t> App::upcomingTrack() const {
    if (music_) return queue_.upcoming();
    if (book_ && fileIndex_ + 1 < book_->files.size()) return fileIndex_ + 1;
    return std::nullopt;
}

void App::setFile(size_t i) {
    if (i != fileIndex_) cancelStarOptions();
    fileIndex_ = i;
    if (music_) queue_.setCurrent(i);
}

std::optional<size_t> App::listIndexOf(const std::string& id, size_t hint) const {
    if (!book_ || !book_->remote) return std::nullopt;
    if (hint < book_->files.size() && book_->files[hint] == id) return hint; // a track can be in a list twice
    for (size_t i = 0; i < book_->files.size(); ++i)
        if (book_->files[i] == id) return i;
    return std::nullopt;
}

std::pair<size_t, double> App::savedPlace(const Book& book) const {
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
    return {file, pos};
}

bool App::openBook(const Book& book, bool autoplay) {
    if (book_) rememberPosition();
    auto [file, pos] = savedPlace(book);
    if (switchTo(book, false, file, pos, autoplay) == OpenResult::Failed) return false; // a local book opens at once
    sleepAtChapterEnd_ = -1;
    return true;
}

// The server book `last_book` names: the item id of a book of a server set up in the index
// (`<server key>:album:<id>`, never a URL), which a local path cannot be. None when that server
// is not set up, `last_book` is a local book, or the book left the index.
const IndexItem* App::savedServerBook() const {
    std::string last = state_.getString(kLastBook);
    if (last.empty()) return nullptr;
    const std::optional<ServerRef> on = serverRefOf({last});
    if (!on || !servers_[on->link].server.configured() || !last.starts_with(LibraryIndex::seenRef(servers_[on->link].key, SeenKind::Album, "")))
        return nullptr;
    const IndexItem* item = index_.find(last);
    return item && item->source == IndexItem::Source::Server && item->kind == IndexItem::Kind::Book ? item : nullptr;
}

// "Now playing" or Play after a restart: opens the server book `last_book` names. False if none.
bool App::resumeServerBook() {
    const IndexItem* saved = savedServerBook();
    if (!saved) return false;
    openServerBook(IndexItem(*saved)); // a copy: opening may change the index
    return true;
}

// `last_book` as a server book of any server names it: an item id `<server key>:album:<id>` is never an absolute path.
bool App::isServerBookId(const std::string& id) {
    return !id.empty() && !std::filesystem::path(id).is_absolute() && id.find(":album:") != std::string::npos;
}

// The chapters of a server book: the tracks of the album, under the item id as its path.
Book App::serverBookOf(const std::string& ref, const std::string& title, const std::string& author,
                       const std::string& albumId, const std::vector<RemoteTrack>& tracks, size_t link,
                       const std::vector<RemoteChapter>& chapters) {
    Book book;
    book.link = link;
    book.title = title;
    book.artist = author;
    book.path = ref;
    book.remote = true;
    book.serverBook = true;
    book.source = RemoteSource{RemoteSource::Kind::Album, albumId};
    for (const RemoteTrack& t : tracks) {
        book.suffixes.push_back(t.suffix);
        book.sizes.push_back(t.size);
        book.files.emplace_back(t.id);
        book.titles.push_back(t.title);
        book.durations.push_back(t.duration);
    }
    book.chapters = serverChaptersOf(book.durations, chapters);
    return book;
}

// The server's chapters on the book's timeline, placed in the files: a file starts where the durations of those before it add
// up to (the same as the server's `startOffset`). A chapter before the start is at the start, one that is not before the end
// of the book is dropped. With a file of unknown length in a book of several files nothing can be placed: none.
std::vector<Book::Chapter> App::serverChaptersOf(const std::vector<double>& durations, std::vector<RemoteChapter> chapters) {
    std::vector<Book::Chapter> out;
    if (chapters.empty() || durations.empty()) return out;
    std::vector<double> offsets;
    double total = 0;
    for (double d : durations) {
        if (d <= 0 && durations.size() > 1) return out;
        offsets.push_back(total);
        total += d;
    }
    std::stable_sort(chapters.begin(), chapters.end(), [](const RemoteChapter& a, const RemoteChapter& b) { return a.start < b.start; });
    for (const RemoteChapter& c : chapters) {
        const double start = c.start;
        if (total > 0 && start >= total - kChapterSlack) continue;
        size_t file = 0;
        while (file + 1 < offsets.size() && offsets[file + 1] <= start + kChapterSlack) ++file;
        Book::Chapter chapter;
        chapter.title = c.title.empty() ? "Chapter " + std::to_string(out.size() + 1) : c.title;
        chapter.file = file;
        chapter.start = std::max(0.0, start - offsets[file]);
        out.push_back(std::move(chapter));
    }
    return out;
}

// A book of the server, from Library or "Now playing" (`last_book`): it plays from where it was left
// (`pos:` + the item id: `<server key>:album:<id>`, no URL) and opens the player. A book the user
// downloaded whole plays from its files at once, with no question to the server and no "Loading."
// (as a resumed download does). Any other is "Loading." while its tracks come, and with the server
// down "Server not reachable." is said and the user stays where they were. The request is the one
// of resumeStream(): a new screen, sleep or power off drops it. Once it plays, the book is
// `last_book`, as for a local book; the music player keeps its own place (`last_stream`). Its place is a file index of the
// list as the server gives it now: a list shorter than the saved index starts at the first chapter.
void App::openServerBook(const IndexItem& item, bool followServer) {
    const std::optional<ServerRef> on = serverRefOf(item.seenOn); // its server: the book is loaded and plays from there
    if (!on) {
        earcons_.play(Earcon::Error);
        say("Server not reachable.");
        return;
    }
    const size_t link = on->link;
    const std::string& ref = on->ref;
    const std::string prefix = LibraryIndex::seenRef(servers_[link].key, SeenKind::Album, ""); // `<key>:album:`
    if (!ref.starts_with(prefix)) {
        earcons_.play(Earcon::Error);
        say("Server not reachable.");
        return;
    }
    std::string albumId = ref.substr(prefix.size());
    cancelResume();
    std::uint64_t screen = screens_.empty() ? 0 : screens_.back().id;
    RemoteSource source{RemoteSource::Kind::Album, albumId};
    const TrackCache& store = trackCacheOf(link);
    CachedList cached = store.enabled() ? store.load(source) : CachedList{};
    std::vector<RemoteTrack> onDisk = completeTracks(store, source, cached);
    if (isWholeDownload(cached, onDisk)) {
        Book book = serverBookOf(ref, item.title, item.artist, albumId, onDisk, link, cached.chapters); // the manifest kept the chapters
        if (book_) rememberPosition(); // the book that plays may be this one: its place is saved only now
        auto [file, pos] = savedPlace(book);
        playServerBook(book, file, pos, [this, screen] { enterPlayerFrom(screen); }, false, followServer);
        return;
    }
    say("Loading.");
    resumeScreen_ = screen; // leaving this screen drops the request (closeScreens())
    resumeLink_ = link;
    // A Books server gives the book (its tracks and chapters); any other the tracks of the album.
    auto opened = [this, screen, ref, title = item.title, author = item.artist, albumId, link, followServer](MusicServer::Error error,
                                                                                            RemoteBook remote) {
        std::vector<RemoteTrack>& tracks = remote.tracks;
        resume_ = 0; // a new screen or sleep would have cancelled it: the user is still where they asked
        resumeScreen_ = 0;
        tts_.stop();
        if (error != MusicServer::Error::None) {
            earcons_.play(Earcon::Error);
            say(errorText(error) + ".");
            return;
        }
        if (tracks.empty()) {
            earcons_.play(Earcon::Error);
            say("This book has no chapters on the server.");
            return;
        }
        Book book = serverBookOf(ref, title, author, albumId, tracks, link, remote.chapters);
        if (book_) rememberPosition();
        auto [file, pos] = savedPlace(book);
        playServerBook(book, file, pos, [this, screen] { enterPlayerFrom(screen); }, false, followServer);
    };
    if (servers_[link].server.catalogue() == MusicServer::Catalogue::Books)
        resume_ = servers_[link].server.book(albumId, [opened](MusicServer::Error error, RemoteBook book) { opened(error, std::move(book)); });
    else
        resume_ = servers_[link].server.albumTracks(albumId, [opened](MusicServer::Error error, std::vector<RemoteTrack> tracks, bool) {
            opened(error, RemoteBook{std::move(tracks), {}});
        });
}

// playMusic() for a book: its chapters are the tracks (the book player) and it keeps its place.
bool App::playServerBook(const Book& book, size_t file, double position, Then opened, bool explicitPlace, bool followServer) {
    if (book_) rememberPosition();
    const bool hadLocal = explicitPlace || !state_.getString("pos:" + book.path.string()).empty() || file != 0 || position != 0;
    const std::string localStamp = state_.getString("posat:" + book.path.string());
    auto playing = [this, hadLocal, localStamp, followServer, opened = std::move(opened)] {
        sleepAtChapterEnd_ = -1;
        pausedAt_.reset();
        if (opened) opened();
        pullProgress(hadLocal, localStamp, followServer);
    };
    return switchTo(book, false, file, position, true, std::move(playing)) != OpenResult::Failed;
}

// Music plays from the chosen track. Local music is never resumed later, so it
// neither saves a position nor replaces the book that start() resumes; a remote
// list keeps its place as last_stream (rememberPosition()).
bool App::playEntries(const Book& list, std::vector<QueueEntry> entries, size_t track, Then opened, double position) {
    resolveCachedPaths(entries);
    Queue queue;
    queue.replace(std::move(entries), track);
    return playKept(list, std::move(queue), std::move(opened), position);
}

void App::resolveCachedPaths(std::vector<QueueEntry>& entries) const {
    for (auto& entry : entries)
        if (entry.kind == QueueEntry::Kind::Local) {
            auto replacement = replacedCacheFiles_.find(entry.path);
            std::error_code ec;
            if (replacement != replacedCacheFiles_.end() && !std::filesystem::exists(entry.path, ec))
                entry.path = replacement->second;
        }
}

bool App::playKept(const Book& list, Queue queue, Then opened, double position) {
    if (book_) rememberPosition();
    // Only once it plays: if it fails, the book before it keeps its sleep timer and rewind.
    auto playing = [this, opened = std::move(opened)] {
        sleepAtChapterEnd_ = -1;
        pausedAt_.reset();
        if (opened) opened();
    };
    const size_t track = queue.position();
    return switchTo(list, true, track, position, true, std::move(playing), std::move(queue)) != OpenResult::Failed;
}

bool App::playMusic(const Book& album, size_t track, Then opened, double position) {
    return playEntries(album, entriesOf(album), track, std::move(opened), position);
}

// The entries of a local list: its files in order, each in the album it belongs to (a list of albums, like
// Play all, has the title of each; any other list is one album). A server list is built in playRemote().
std::vector<QueueEntry> App::entriesOf(const Book& list) {
    std::vector<QueueEntry> entries;
    for (size_t i = 0; i < list.files.size(); ++i) {
        QueueEntry e;
        e.path = list.files[i].string();
        if (i < list.titles.size() && !list.titles[i].empty()) e.title = list.titles[i];
        else e.title = chapterTitle(list.files[i]);
        e.album = list.title;
        for (const Book::Album& album : list.albums)
            if (album.first <= i) e.album = album.title;
        e.artist = list.artist;
        entries.push_back(std::move(e));
    }
    return entries;
}

bool App::playQueue(const std::string& title, std::vector<QueueEntry> entries, size_t start) {
    if (start >= entries.size()) {
        earcons_.play(Earcon::Error);
        say("Nothing to play.");
        return false;
    }
    Book list;
    list.title = title;
    std::uint64_t screen = screens_.empty() ? 0 : screens_.back().id;
    return playEntries(list, std::move(entries), start, [this, screen] { enterPlayerFrom(screen); });
}

Book App::remoteBook(const std::string& title, const std::vector<RemoteTrack>& tracks, std::optional<RemoteSource> source,
                     const std::string& artist, size_t link) {
    Book list;
    list.link = link;
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
    return list;
}

// A server album is one album, whatever its tracks say (a disc 2 tag); a playlist has the albums of its tracks.
std::vector<QueueEntry> App::remoteEntries(const std::string& title, const std::vector<RemoteTrack>& tracks, bool ofAlbum,
                                           const std::string& artist, const std::string& albumId, size_t link) const {
    // The entry names its server by url and user, as queue.tsv keeps it: never a password or a stream URL.
    const ServerConfig config = link < servers_.size() ? servers_[link].config : ServerConfig{};
    std::vector<QueueEntry> entries;
    for (const RemoteTrack& t : tracks) {
        QueueEntry e;
        e.kind = QueueEntry::Kind::Remote;
        e.serverUrl = config.url;
        e.user = config.user;
        e.trackId = t.id;
        e.albumId = ofAlbum && !albumId.empty() ? albumId : t.albumId;
        e.title = t.title;
        e.album = ofAlbum || t.album.empty() ? title : t.album;
        e.artist = t.artist.empty() ? artist : t.artist;
        e.duration = t.duration;
        entries.push_back(std::move(e));
    }
    return entries;
}

bool App::playRemote(const std::string& title, const std::vector<RemoteTrack>& tracks, size_t index,
                     std::optional<RemoteSource> source, double position, const std::string& artist, size_t link) {
    if (index >= tracks.size()) {
        earcons_.play(Earcon::Error);
        say("Nothing to play.");
        return false;
    }
    const bool ofAlbum = source && source->kind == RemoteSource::Kind::Album;
    const std::string albumId = ofAlbum ? source->id : std::string();
    Book list = remoteBook(title, tracks, std::move(source), artist, link);
    std::vector<QueueEntry> entries = remoteEntries(title, tracks, ofAlbum, artist, albumId, link);
    std::uint64_t screen = screens_.empty() ? 0 : screens_.back().id;
    return playEntries(list, std::move(entries), index, [this, screen] { enterPlayerFrom(screen); }, position);
}

std::optional<App::PreviousQueue> App::editedQueue() const {
    if (book_ && music_ && queue_.edited()) return PreviousQueue{*book_, queue_, place()};
    if (parked_ && parked_->music && parked_->queue.edited()) return PreviousQueue{parked_->book, parked_->queue, parked_->position};
    return std::nullopt;
}

// OK on "Previous queue": it replaces the queue (which is kept in turn if it was edited) and plays its
// current entry from where it stopped. Nothing is left to restore when the queue it replaced was not edited.
void App::restorePrevious() {
    if (!previous_) return;
    PreviousQueue back = *previous_;
    const bool keepsReplaced = editedQueue().has_value();
    std::uint64_t screen = screens_.back().id;
    playKept(back.list, std::move(back.queue), [this, keepsReplaced, screen] {
        if (!keepsReplaced) previous_.reset();
        enterPlayerFrom(screen);
    }, back.position);
}

// Loads `book` into the music player (`music`) or the book player from `file`. If none of its
// files from there on opens, what was loaded before stays loaded, at the same place, so the user
// is where they were. The other player's book is parked with its place, so Continue brings it
// back; a book of the same kind replaces the loaded one and leaves the parked one alone.
App::OpenResult App::switchTo(const Book& book, bool music, size_t file, double position, bool autoplay,
                              Then opened, std::optional<Queue> queue, bool continuing) {
    cancelProgressPull();
    pushProgress(); // the book being replaced keeps its last heard place on its server
    progressBook_.clear();
    progressHeardSince_.reset();
    progressHeard_ = {};
    dropPendingOpen(); // a failure goes back to what was loaded before it
    cancelMarkPing();  // Play's answer is for the queue that was loaded
    // A queue edited by hand that this one replaces is kept once this one has opened.
    std::optional<PreviousQueue> kept;
    if (music && queue && !continuing) kept = editedQueue();
    size_t wasFile = fileIndex_;
    bool wasOpen = player_.isOpen(), wasPlaying = player_.playing();
    double wasAt = player_.position();
    std::optional<Parked> wasParked = parked_;
    if (book_ && music_ != music) parked_ = Parked{*book_, music_, fileIndex_, place(), music_ ? queue_ : Queue{}};
    auto undo = [this, previous = std::exchange(book_, book), wasMusic = std::exchange(music_, music),
                 wasQueue = std::exchange(queue_, music && queue ? std::move(*queue) : Queue{}), wasFile, wasOpen,
                 wasPlaying, wasAt, wasParked = std::move(wasParked)] {
        const bool failedRemote = trackRemote(fileIndex_); // the track of the new list that did not open
        book_ = previous;
        music_ = wasMusic;
        queue_ = wasQueue;
        fileIndex_ = wasFile;
        parked_ = wasParked;
        if (!book_ || !wasOpen) return;
        if (trackRemote(wasFile) && failedRemote) {
            reopenAt_ = wasAt; // its server is likely gone too: open it on Play, not now
        } else if (trackRemote(wasFile)) {
            // A local file failed: the server had nothing to do with it. The track opens
            // again in the background; if that fails, Play tries it again.
            openFile(wasFile, wasAt, wasPlaying, Origin::Restore, {}, [this, wasAt] { reopenAt_ = wasAt; });
        } else if (player_.open(trackKey(wasFile), wasAt)) {
            if (wasPlaying && !failedRemote) player_.play(); // the server is gone: stay paused
        } else {
            book_.reset(), music_ = false, queue_ = Queue{};
        }
    };
    auto playing = [this, music, kept = std::move(kept), path = book.path.string(), opened = std::move(opened)] {
        if (kept) previous_ = kept;
        if (music) { // the queue is kept in queue.tsv now: a stream saved by an older version is out of date
            state_.clearPrefix(kLastStreamPrefix);
            restoredQueue_ = false;
        }
        state_.setString(kLastPlayer, music ? kPlayerMusic : kPlayerBook);
        if (!music) state_.setString(kLastBook, path); // start() and "Now playing" resume it
        if (opened) opened();
    };
    return openFile(file, position, autoplay, Origin::Menu, std::move(playing), std::move(undo));
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
        // A remote track failed, or the one that was playing is a remote one that the failed local
        // open has closed: it is the current track again, opened on Play.
        if (book_ && (trackRemote(fileIndex_) || trackRemote(wasFile))) {
            setFile(wasFile);
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
App::OpenResult App::openFile(size_t file, double position, bool autoplay, Origin origin, Then opened, Then undo,
                              SkippedRun skipped) {
    cancelNavigation();
    seekAtEnd_ = false;
    if (!book_) return OpenResult::Failed;
    // With Repeat all the entries after the last are the first ones again (not for a track chosen in a list,
    // which must not end up at the start): the scan wraps, and tries each entry once, so a queue with
    // nothing that plays ends instead of going round for ever.
    const bool wraps = music_ && origin != Origin::Menu && queue_.repeat() == QueueRepeat::All && trackCount() > 0;
    if (wraps) file %= trackCount();
    // With Repeat album, at the end of a track, the entries to try are those of the album (Repeat album plays
    // only that album): the ring from `file`, each once. Other ways to a track move through the play order.
    const bool inAlbum = music_ && origin == Origin::Next && queue_.repeat() == QueueRepeat::Album && file < trackCount();
    std::vector<size_t> ring;
    if (inAlbum) ring = queue_.albumRing(file);
    const size_t startFile = file;
    const double startPosition = position;
    // (`skipped` may hold entries already tried by the call that went on after a failure.)
    const size_t end = inAlbum  ? ring.size() - std::min(skipped.count, ring.size())
                       : wraps ? startFile + trackCount() - std::min(skipped.count, trackCount())
                               : trackCount();
    for (size_t step = inAlbum ? 0 : startFile; step < end; ++step, position = 0) {
        file = inAlbum ? ring[step] : wraps ? step % trackCount() : step;
        // A music queue skips what cannot play; the run is said once, when one plays.
        if (music_)
            if (Availability why = availability(file, origin); why != Availability::Yes) {
                addSkipped(skipped, file, why);
                continue;
            }
        sayUnavailable(skipped);
        setFile(file);
        openedFromStream_ = false;
        bool opens = false;
        bool cachedFallback = false;
        const bool remote = trackRemote(file);
        const std::string key = trackKey(file);
        const size_t link = trackLink(file).value_or(0); // (a music entry that gets here has a server)
        if (remote) {
            // A downloaded track is a local file: it opens at once and seeks by itself. If it
            // cannot be opened after all, the stream is the way.
            if (std::optional<std::filesystem::path> cached = cachedFile(link, key, true)) {
                cachedFallback = upgradeCached(link, *cached, streamingBitrate());
                if (!cachedFallback) opens = player_.open(cached->string(), position);
                if (!opens && !cachedFallback && linkState(link).cacheIndex) linkState(link).cacheIndex->erase(key); // not tried again at every play
            }
            if (!opens) {
                // A stream takes a moment to open: say why nothing plays yet.
                if (origin == Origin::Menu || origin == Origin::Key || origin == Origin::Resume) say("Loading.");
                MusicServer::Stream stream = remoteStream(file);
                openedStreamBitrate_ = stream.transcoded ? streamingBitrate() : kOriginalQuality;
                openedFromStream_ = true;
                opens = player_.open(stream.url, position, stream.transcoded);
                if (opens && !player_.opening() && music_) clearServerDown(link); // the stream opened at once
                if (!opens && cachedFallback && openCachedFallback(file, position, autoplay, origin, opened))
                    return OpenResult::Opened;
            }
        } else {
            opens = player_.open(key, position);
        }
        if (opens) {
            if (player_.opening()) {
                std::uint64_t screen = origin == Origin::Menu && !screens_.empty() ? screens_.back().id : 0;
                pendingOpen_ = PendingOpen{screen,    origin,         trackTitle(file), autoplay,
                                           position,  clock_.now(),   std::move(opened),       std::move(undo), file, cachedFallback};
                return OpenResult::Pending;
            }
            onOpened(autoplay, origin, opened);
            return OpenResult::Opened;
        }
        if (remote && continuesAfterFailure(origin)) { // the next entry is tried, or the queue stops
            markServerDown(link);
            addSkipped(skipped, file, Availability::ServerDown);
            continue;
        }
        if (remote) {
            onCannotPlay(trackTitle(file), origin == Origin::Restore, undo);
            return OpenResult::Failed;
        }
        earcons_.play(Earcon::Error);
        say("Cannot open " + std::filesystem::path(key).filename().string());
    }
    if (skipped.count) {
        // Nothing from there on plays. Where the server is the reason, the queue waits at the first entry
        // that was skipped, for Play to try the server again.
        const bool parks = skipped.serverDown && (origin == Origin::Next || origin == Origin::Resume);
        if (parks) return parkAtSkipped(skipped, startFile, startPosition);
        sayUnavailable(skipped);
    }
    if (undo) undo();
    return OpenResult::Failed;
}

// A music queue entry plays only if its file is there, or it is of a server set up now (url and user) and
// that server is not marked down or the track is downloaded. Menu tries a server that is marked down: the user chose from what it just
// listed, and an open that works clears the mark.
App::Availability App::availability(size_t i, Origin origin) const {
    const QueueEntry& e = queue_.at(i);
    if (e.kind == QueueEntry::Kind::Local) {
        std::error_code ec;
        return std::filesystem::exists(e.path, ec) ? Availability::Yes : Availability::Gone;
    }
    const std::optional<size_t> link = linkOf(e);
    if (!link) return Availability::NoServer;
    if (!serverDown(*link) || origin == Origin::Menu) return Availability::Yes;
    // A downloaded track plays from its file, whatever the server does.
    return cachedFile(*link, e.trackId, true) ? Availability::Yes : Availability::ServerDown;
}

void App::addSkipped(SkippedRun& skipped, size_t file, Availability why) {
    if (!skipped.first) {
        skipped.first = file;
        skipped.firstTitle = trackTitle(file);
    }
    ++skipped.count;
    if (why == Availability::ServerDown) skipped.serverDown = true;
    if (why == Availability::NoServer) ++skipped.noServer;
}

void App::sayUnavailable(SkippedRun& skipped) {
    if (!skipped.count) return;
    earcons_.play(Earcon::Error);
    const std::string what = skipped.count == 1 ? skipped.firstTitle : std::to_string(skipped.count) + " tracks";
    // A run whose servers were all removed from the config says so; any other mix is just unavailable.
    if (skipped.noServer == skipped.count) say("Cannot play " + what + ". Server not set up.");
    else say(what + ", source unavailable.");
    skipped = {};
}

// The queue waits at the first skipped entry, closed: Play pings the server and opens it if it answers.
App::OpenResult App::parkAtSkipped(const SkippedRun& skipped, size_t startFile, double startPosition) {
    const size_t first = *skipped.first;
    player_.pause();
    player_.close();
    setFile(first);
    reopenAt_ = first == startFile ? startPosition : 0.0;
    rememberPosition();
    earcons_.play(Earcon::Error);
    say("Server not reachable. Press play to try again.");
    return OpenResult::Stopped;
}

// At the end of a track the next one opens quietly. If a server track does not open, the server is marked
// down and the queue goes on with the entries after it that can play, or stops there.
bool App::continuesAfterFailure(Origin origin) const { return music_ && origin == Origin::Next; }

void App::continueAfterFailure(size_t file, const std::string& title) {
    SkippedRun skipped;
    skipped.first = file;
    skipped.firstTitle = title;
    skipped.count = 1;
    skipped.serverDown = true;
    // (Repeat album: the album's next entry, not the next one of the play order.)
    const bool inAlbum = queue_.repeat() == QueueRepeat::Album && file < trackCount();
    openFile(inAlbum ? queue_.albumNext(file) : file + 1, 0, true, Origin::Next, {}, {}, skipped);
}

void App::onOpened(bool autoplay, Origin origin, const Then& opened) {
    reopenAt_.reset();
    restoredQueue_ = false;
    // The current track opened again (Play after it broke off, or it is back
    // behind a choice that failed) goes on with its play.
    bool goesOn = scrobble_ && (origin == Origin::Resume || origin == Origin::Restore);
    if (!trackRemote(fileIndex_)) scrobble_.reset();
    else if (!goesOn) startTrackPlay();
    if (autoplay) {
        player_.play();
        if (book_ && state_.getString("finished:" + book_->path.string()) == "1") recordProgress();
    }
    if (opened) opened();
}

void App::onCannotPlay(const std::string& title, bool quiet, const Then& undo) {
    if (music_)
        if (const std::optional<size_t> link = trackLink(fileIndex_)) markServerDown(*link); // the track of the queue that did not open
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
    cancelNavigation(); // contacts pressed while loading must not act on the new file
    PendingOpen open = std::move(*pendingOpen_);
    pendingOpen_.reset(); // before the callbacks, which may open again
    if (player_.isOpen()) {
        if (music_)
            if (const std::optional<size_t> link = trackLink(open.file)) clearServerDown(*link); // a stream opened: the server answers
        onOpened(open.autoplay, open.origin, open.opened);
    } else if (!open.cachedFallback || !openCachedFallback(open.file, open.position, open.autoplay, open.origin, open.opened)) {
        failOpen(open.origin, open.title, open.file, open.undo);
    }
}

// A stream did not open. At the end of a track of the music queue that is not an error to stop on: the
// server is marked down and the queue goes on with what can play.
void App::failOpen(Origin origin, const std::string& title, size_t file, const Then& undo) {
    if (continuesAfterFailure(origin)) {
        if (const std::optional<size_t> link = trackLink(file)) markServerDown(*link);
        continueAfterFailure(file, title);
        return;
    }
    onCannotPlay(title, origin == Origin::Restore, undo);
}

void App::giveUpPendingOpen() {
    PendingOpen open = std::move(*pendingOpen_);
    pendingOpen_.reset();
    player_.close(); // drops the open: its result never shows
    if (!open.cachedFallback || !openCachedFallback(open.file, open.position, open.autoplay, open.origin, open.opened))
        failOpen(open.origin, open.title, open.file, open.undo);
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

// The seconds Play goes back after a pause of more than 10 s (the setting), else 0.
int App::resumeRewind() const {
    int rewind = config_.getInt(kRewindOnResume, 2);
    return rewind > 0 && pausedAt_ && clock_.now() - *pausedAt_ > std::chrono::seconds(10) ? rewind : 0;
}

void App::play() {
    seekAtEnd_ = false;
    if (book_ && !player_.isOpen() && reopenAt_) {
        // The track cannot be opened now (its server is marked down, or it is a file that is gone with the
        // server's tracks behind it): Play asks the server first, and goes on once it answers.
        if (const std::optional<size_t> link = linkToCheck(); link && availability(fileIndex_, Origin::Key) != Availability::Yes) {
            say("Checking the server.");
            startMarkPing(*link, true);
            return;
        }
        resumeClosedTrack();
        return;
    }
    if (!book_) { // after a restart: like "Now playing" on Home
        if (parked_) continueParked();
        else if (!resumePlayer(lastPlayerIsMusic()) && !resumePlayer(!lastPlayerIsMusic())) nothingToPlay();
        return;
    }
    if (!player_.isOpen()) {
        nothingToPlay();
        return;
    }
    if (int rewind = resumeRewind()) player_.seek(player_.position() - rewind);
    pausedAt_.reset();
    player_.play();
    if (book_ && state_.getString("finished:" + book_->path.string()) == "1") recordProgress();
    earcons_.play(Earcon::Play);
}

// ---- a server that is marked down

App::LinkState& App::linkState(size_t link) const { return links_[link < servers_.size() ? link : 0]; }

bool App::serverDown(size_t link) const { return link < servers_.size() && linkState(link).down; }

void App::markServerDown(size_t link) {
    if (link >= servers_.size()) return;
    LinkState& state = linkState(link);
    if (state.down) return; // another failure does not restart the pings at 10 s
    state.down = true;
    state.pingStep = 0;
    state.pingDue = clock_.now() + kPingFirst;
}

void App::clearServerDown(size_t link) {
    if (link < servers_.size()) linkState(link).down = false;
    sendScrobbles(); // it answers
}

// Play at a track that cannot open: the server to ask first. The current track's own server if it is marked
// down, else any server marked down that has entries ahead (the queue waits at a missing file before them).
std::optional<size_t> App::linkToCheck() {
    if (!book_ || !music_) return std::nullopt;
    if (const std::optional<size_t> own = trackLink(fileIndex_); own && serverDown(*own) && remoteAhead(*own)) return own;
    for (size_t link = 0; link < servers_.size(); ++link)
        if (serverDown(link) && remoteAhead(link)) return link;
    return std::nullopt;
}

// ---- the scrobble outbox

// A submission is kept first, then sent: it leaves the outbox only when the server answers it.
void App::submitScrobble(size_t link, const std::string& trackId) {
    if (link >= servers_.size() || servers_[link].key.empty() || !servers_[link].server.configured()) return;
    outbox_.push_back({servers_[link].key, trackId, static_cast<std::int64_t>(clock_.wallTime()) * kProgressMillisPerSecond});
    if (outbox_.size() > kMaxScrobbles) outbox_.erase(outbox_.begin(), outbox_.end() - kMaxScrobbles);
    saveScrobbles();
    sendScrobbles();
}

void App::loadScrobbles() {
    if (scrobbleFile_.empty() || !textFiles_) return;
    const std::optional<std::string> text = textFiles_->read(scrobbleFile_);
    if (!text) return;
    scrobbleText_ = *text;
    outbox_ = parseScrobbles(*text);
}

void App::saveScrobbles() {
    if (scrobbleFile_.empty() || !textFiles_) return;
    const std::string text = formatScrobbles(outbox_);
    if (text == scrobbleText_) return;
    if (textFiles_->writeAtomically(scrobbleFile_, text)) scrobbleText_ = text; // else the next change tries again
}

// One at a time, oldest first, with the time it was heard. Called whenever the server answers.
void App::sendScrobbles() {
    scrobbleFailed_.clear(); // a server answered, or something new is waiting: every server is tried again
    sendNextScrobble();
}

void App::sendNextScrobble() {
    if (scrobbleOut_ || shutDown_) return;
    // The first server, in config order, that is up and has an entry waiting; each entry goes to its own server.
    for (size_t link = 0; link < servers_.size(); ++link) {
        if (serverDown(link) || scrobbleFailed_.count(link) || !servers_[link].server.configured() || servers_[link].key.empty()) continue;
        const std::string& key = servers_[link].key;
        const auto next = std::find_if(outbox_.begin(), outbox_.end(), [&](const ScrobbleEntry& e) { return e.server == key; });
        if (next == outbox_.end()) continue;
        scrobbleOut_ = true;
        servers_[link].server.scrobble(next->trackId, true, next->timeMs,
                                       [this, sent = *next](MusicServer::Error error) { onScrobbleAnswer(sent, error); });
        return;
    }
}

// The entry leaves the outbox when the server took it, or refused it (Server: not sent for ever). Not
// reaching the server, a login that fails or no server set up keep it and stop the round: the next
// answer from the server starts it again.
void App::onScrobbleAnswer(const ScrobbleEntry& sent, MusicServer::Error error) {
    scrobbleOut_ = false;
    if (error != MusicServer::Error::None && error != MusicServer::Error::Server) {
        // That server stops the round, but the entries of the others still go (it may not be marked down).
        for (size_t link = 0; link < servers_.size(); ++link)
            if (servers_[link].key == sent.server) scrobbleFailed_.insert(link);
        sendNextScrobble();
        return;
    }
    if (const auto it = std::find(outbox_.begin(), outbox_.end(), sent); it != outbox_.end()) outbox_.erase(it);
    saveScrobbles();
    for (size_t link = 0; link < servers_.size(); ++link)
        if (servers_[link].key == sent.server) scrobbleFailed_.erase(link); // it answered
    sendNextScrobble(); // (a server that failed before waits for its own answer, not for this one)
}

// Entries that can come up that belong to server `link`.
bool App::remoteAhead(size_t link) const {
    if (!book_ || !music_ || link >= servers_.size() || !servers_[link].server.configured()) return false;
    const std::string& url = servers_[link].config.url;
    const std::string& user = servers_[link].config.user;
    // With Repeat all or album the entries before the current one come up again (after the last / the album's last).
    const size_t from = queue_.repeat() == QueueRepeat::All || queue_.repeat() == QueueRepeat::Album ? 0 : fileIndex_;
    for (size_t i = from; i < queue_.size(); ++i)
        if (const QueueEntry& e = queue_.at(i); e.kind == QueueEntry::Kind::Remote && e.serverUrl == url && e.user == user)
            return true;
    return false;
}

bool App::remoteAheadMemo(size_t link) {
    AheadMemo& m = linkState(link).ahead;
    const bool configured = serverAt(link).configured();
    if (!m.known || m.revision != queue_.revision() || m.file != fileIndex_ || m.music != music_ || m.loaded != !!book_ ||
        m.configured != configured) {
        m = AheadMemo{queue_.revision(), fileIndex_, music_, !!book_, configured, true, remoteAhead(link)};
    }
    return m.ahead;
}

// tick(): while the server is marked down and the music player has its entries ahead, ping it when due. With
// none ahead the pings stop and the schedule starts again (10 s) when there are.
void App::checkServerMark(TimePoint now) {
    for (size_t link = 0; link < servers_.size(); ++link) {
        LinkState& state = linkState(link);
        if (!state.down) continue;
        if (sleeping_ || !remoteAheadMemo(link)) {
            state.pingDue = now + kPingFirst;
            state.pingStep = 0;
            continue;
        }
        if (!state.markPing && now >= state.pingDue) startMarkPing(link, false);
    }
}

void App::startMarkPing(size_t link, bool forPlay) {
    if (linkState(link).markPing) serverAt(link).cancel(std::exchange(linkState(link).markPing, 0)); // (another server's ping goes on)
    linkState(link).markPing = serverAt(link).ping([this, link, forPlay](MusicServer::Error error, MusicServer::ServerInfo) {
        linkState(link).markPing = 0;
        onMarkPing(link, error == MusicServer::Error::None, forPlay);
    });
}

// A cancelled ping never calls back (MusicServer): a new queue, sleep and power off drop Play's.
void App::cancelMarkPing() {
    for (size_t link = 0; link < servers_.size(); ++link)
        if (linkState(link).markPing) serverAt(link).cancel(std::exchange(linkState(link).markPing, 0));
}

void App::onMarkPing(size_t link, bool answered, bool forPlay) {
    if (answered) clearServerDown(link);
    else {
        LinkState& state = linkState(link);
        const auto wait = state.pingStep == 0 ? kPingSecond : kPingEvery; // the pause after this one
        ++state.pingStep;
        state.pingDue = clock_.now() + wait;
    }
    if (!forPlay) return;
    // Play: the track is opened now (skipped, if the server does not answer), unless the user has moved on.
    if (book_ && music_ && reopenAt_ && !player_.isOpen() && !pendingOpen_) resumeClosedTrack();
}

// Play on a track that is closed (it failed, the stream broke off, or the queue ended): it opens again at
// its place. If it fails, "Server not reachable" is said, and Play tries again at the same place.
void App::resumeClosedTrack() {
    double at = std::max(0.0, *reopenAt_ - resumeRewind());
    pausedAt_.reset();
    openFile(fileIndex_, at, true, Origin::Resume, [this] { if (player_.playing()) earcons_.play(Earcon::Play); }, {});
}

void App::nothingToPlay() {
    earcons_.play(Earcon::Error);
    say("Nothing to play. Choose a book in the library.");
}

// The player used last, from state.tsv. Without a record (an older state.tsv) it is the music
// player when a music queue or a stream is saved, as that was the one that played last then.
bool App::lastPlayerIsMusic() const {
    std::string last = state_.getString(kLastPlayer);
    return last == kPlayerMusic || (last.empty() && ((book_ && music_) || (parked_ && parked_->music) || savedStream()));
}

// What the player of that kind has for Home: its loaded or parked book, else the one saved for
// after a restart (the stream of the music player, `last_book` of the book player).
std::optional<std::string> App::playerTitle(bool music) const {
    if (book_ && music_ == music) return book_->title;
    if (parked_ && parked_->music == music) return parked_->book.title;
    if (music) {
        if (auto stream = savedStream()) return stream->title;
        return std::nullopt;
    }
    if (const IndexItem* saved = savedServerBook()) return saved->title;
    // A local book the index knows: Home is rebuilt often, so it never scans the disk for one.
    const std::string last = state_.getString(kLastBook);
    if (!last.empty() && index_.find(kLocalBookPrefix + last))
        if (auto local = resumableBook(last)) return local->title;
    return std::nullopt;
}

// Home: the loaded player is entered, a parked one brought back, a saved one resumed.
void App::openPlayer(bool music) {
    if (book_ && music_ == music && music && restoredQueue_ && !player_.isOpen() && !pendingOpen_ && reopenAt_) {
        // The queue of a restart: choosing it plays it from its place, as a saved stream did. Until a
        // track has opened (onOpened()) it stays so, and choosing it again tries again.
        const std::uint64_t screen = screens_.empty() ? 0 : screens_.back().id;
        openFile(fileIndex_, *reopenAt_, true, Origin::Menu, [this, screen] { enterPlayerFrom(screen); }, {});
    } else if (book_ && music_ == music) {
        enterPlayer();
        pullProgress(!state_.getString("pos:" + book_->path.string()).empty() || bookTime(fileIndex_, place()) > 0,
                     state_.getString("posat:" + book_->path.string()));
    } else if (parked_ && parked_->music == music) continueParked();
    else resumePlayer(music);
}

// Loads the place that player has saved after a restart. False if it has none.
bool App::resumePlayer(bool music) {
    if (music) {
        if (!savedStream()) return false;
        resumeStream();
        return true;
    }
    if (resumeServerBook()) return true;
    auto local = resumableBook(state_.getString(kLastBook));
    if (!local) return false;
    std::uint64_t screen = screens_.empty() ? 0 : screens_.back().id;
    if (!openBook(*local, true)) return false; // the error is said; the other player may do
    enterPlayerFrom(screen);
    return true;
}

// Continue: the parked player plays again from the place it stopped at, and the loaded one is
// parked in its turn. There is one audio output, so its file is opened again.
void App::continueParked() {
    if (!parked_) return;
    Parked target = *parked_;
    if (book_) rememberPosition();
    std::uint64_t screen = screens_.empty() ? 0 : screens_.back().id;
    switchTo(target.book, target.music, target.file, target.position, true,
             [this, screen] {
                 sleepAtChapterEnd_ = -1;
                 pausedAt_.reset();
                 enterPlayerFrom(screen);
                 pullProgress(true, state_.getString("posat:" + book_->path.string()));
             },
             target.music ? std::optional<Queue>(std::move(target.queue)) : std::nullopt, true);
}

void App::pause(bool announce) {
    if (!player_.playing()) return;
    player_.pause();
    pausedAt_ = clock_.now();
    rememberPosition();
    pushProgress();
    if (announce) say("Paused.");
}

// Hold seeking stays in one file. Keep our requested target: decoder feedback may
// lag while a stream reopens, and a stage change must not add an immediate step.
void App::startSeeking(Button button, bool deep) {
    if (seeking_ && seeking_->button == button) {
        // Long still arrives after an immediate Deep; it must not ease the rate.
        if (deep) seeking_->deep = true;
        return;
    }
    finishSeeking(false);
    if (!book_ || pendingOpen_ || !player_.isOpen() || player_.failed()) {
        cancelNavigation(); // a late stage/hold cannot start once the file finally opens
        earcons_.play(Earcon::Edge);
        return;
    }
    seeking_ = Seeking{button, deep, false, player_.position(), clock_.now() + kSeekInterval};
    earcons_.play(Earcon::Move);
    seekStep();
}

void App::seekStep() {
    if (!seeking_) return;
    Seeking& seek = *seeking_;
    const int direction = seek.button == Button::Back ? -1 : 1;
    const double duration = player_.duration();
    const double previous = seek.target;
    const double requested = seek.target + direction * (seek.deep ? kDeepSeekStep : kSeekStep);
    seek.target = std::max(0.0, requested);
    if (duration > 0) seek.target = std::min(seek.target, duration);
    seekAtEnd_ = duration > 0 && seek.target >= duration;
    const bool edge = seek.target <= 0 || seekAtEnd_;
    if (edge && !seek.edgeHeard) earcons_.play(Earcon::Edge);
    seek.edgeHeard = edge;
    if (seek.target != previous) player_.seek(seek.target);
}

void App::repeatSeeking(TimePoint now) {
    if (!seeking_) return;
    if (mode() != Mode::Player || pendingOpen_ || !player_.isOpen() || player_.failed()) {
        cancelGestures();
        return;
    }
    if (now >= seeking_->next) {
        lastActivity_ = now; // a paused player is still being used while its key is held
        seeking_->next = now + kSeekInterval;
        seekStep();
    }
}

void App::finishSeeking(bool announce) {
    if (!seeking_) return;
    const double target = seeking_->target;
    if (player_.finished()) seekAtEnd_ = true;
    seeking_.reset();
    if (announce) {
        tts_.stop();
        say("Position, " + spokenDuration(target) + ".");
    }
}

// The edge sound plays once per press: a held key repeating at its limit stays quiet.
void App::changeVolume(int delta, Gesture gesture) {
    int old = config_.getInt(kVolume, 70);
    int v = std::clamp(old + delta, 0, 100);
    if (v == old) {
        if (gesture != Gesture::Repeat || !volumeEdgeHeard_) earcons_.play(Earcon::Edge);
        volumeEdgeHeard_ = true;
        if (gesture != Gesture::Repeat) say(v == 100 ? "Maximum volume." : "Muted.");
        return;
    }
    volumeEdgeHeard_ = false;
    tts_.stop();
    config_.setInt(kVolume, v);
    volumeChanged_ = true; // a config.yaml write per press would wear the flash
    volume_.setVolume(v / 100.0f);
    earcons_.play(Earcon::Volume);
    if (v == 100) say("Maximum volume.");
    else if (v == 0) say("Muted.");
    else if (!player_.playing()) say("Volume " + std::to_string(v) + " percent.");
}

bool App::hasServerChapters() const { return !music_ && book_ && !book_->chapters.empty(); }

// Seconds from the start of the book to `at` seconds into file `file`, by the lengths the server gave.
double App::bookTime(size_t file, double at) const {
    double total = at;
    for (size_t i = 0; i < file && i < book_->durations.size(); ++i) total += book_->durations[i];
    return total;
}

// Seconds from the position to the end of the book (the open file's own length, then the files after it).
double App::bookLeft() const {
    // A stream that does not say its length (a transcode) has the server's.
    double length = player_.duration() > 0 ? player_.duration() : fileIndex_ < book_->durations.size() ? book_->durations[fileIndex_] : 0;
    double left = std::max(0.0, length - player_.position());
    for (size_t i = fileIndex_ + 1; i < book_->durations.size(); ++i) left += book_->durations[i];
    return left;
}

// How long chapter `index` of the list has played. A server chapter may have begun in an earlier file.
double App::secondsIntoChapter(const ChapterRef& ch) const {
    if (!hasServerChapters()) return player_.position() - ch.start;
    return bookTime(fileIndex_, player_.position()) - bookTime(ch.file, ch.start);
}

// The file that ended is the end of the chapter too, unless a server chapter goes on in the next file.
bool App::chapterEndsWithFile() const {
    if (!hasServerChapters()) return true;
    const auto& list = book_->chapters;
    const size_t next = size_t(currentChapter()) + 1;
    if (next >= list.size()) return !upcomingTrack();
    // The next chapter is in this file when the file is shorter than the server said: nothing more of this chapter follows.
    return list[next].file <= fileIndex_ || (list[next].file == fileIndex_ + 1 && list[next].start <= kChapterSlack);
}

// A file is a chapter: the files of a book that lists no chapters, the entries of a queue.
bool App::filePerChapter() const { return trackCount() > 1 && !hasServerChapters(); }

std::vector<App::ChapterRef> App::chapters() const {
    std::vector<ChapterRef> out;
    if (!book_) return out;
    if (hasServerChapters()) {
        for (const Book::Chapter& c : book_->chapters) out.push_back({c.title, c.file, c.start});
    } else if (trackCount() > 1) {
        for (size_t i = 0; i < trackCount(); ++i)
            out.push_back({trackTitle(i), i, 0});
    } else if (!player_.chapters().empty()) {
        for (const Chapter& c : player_.chapters()) out.push_back({c.title, 0, c.start});
    } else {
        out.push_back({trackRemote(0) ? trackTitle(0) : book_->title, 0, 0});
    }
    return out;
}

int App::currentChapter() const {
    if (!book_) return 0;
    if (filePerChapter()) return int(fileIndex_);
    int cur = 0;
    if (hasServerChapters()) {
        const double pos = place() + kChapterPositionSlack; // a closed player (a failed stream) still has a place // the last one that starts at or before the position, in a file before this or in this
        const auto& list = book_->chapters;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].file < fileIndex_ || (list[i].file == fileIndex_ && list[i].start <= pos)) cur = int(i);
        return cur;
    }
    const double pos = player_.position() + kChapterPositionSlack;
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
    seekAtEnd_ = false;
    auto arrived = [this, index, opened = std::move(opened)] {
        // The track that plays, which is a later one when the queue skipped entries that cannot play.
        if (sleepAtChapterEnd_ >= 0) sleepAtChapterEnd_ = filePerChapter() ? int(fileIndex_) : index;
        if (opened) opened();
    };
    // Closed: a remote track that failed, ended the list or is still opening.
    if (ch.file != fileIndex_ || !player_.isOpen())
        return loadFile(ch.file, ch.start, willPlay(), origin, std::move(arrived)) != OpenResult::Failed;
    player_.seek(ch.start);
    // The whole track from its start (not a chapter in it): heard again.
    if (trackRemote(fileIndex_) && (filePerChapter() || (!hasServerChapters() && player_.chapters().empty()))) startTrackPlay();
    arrived();
    return true;
}

void App::stepChapter(int direction) {
    seekAtEnd_ = false;
    auto list = chapters();
    int cur = currentChapter();
    int target = cur + direction;
    if (direction < 0) {
        // Like a CD player: first press goes to the start of the current chapter.
        if (secondsIntoChapter(list[size_t(cur)]) > kPreviousRestartAfter) target = cur;
    }
    // With Repeat all Previous on the first track goes to the last, and Next on the last to the first.
    const int count = int(list.size());
    const bool wraps = music_ && count > 1 && queue_.repeat() == QueueRepeat::All;
    const bool goingBack = music_ && count > 1 && target < cur;
    if (wraps) target = (target + count) % count;
    // Previous goes back over the entries of the queue that cannot play (Next skips them when it opens one).
    SkippedRun skipped;
    if (goingBack) {
        for (int tried = 0; target >= 0 && availability(size_t(target), Origin::Key) != Availability::Yes; ++tried) {
            if (tried == count) { // none plays: said once, here
                sayUnavailable(skipped);
                return;
            }
            addSkipped(skipped, size_t(target), availability(size_t(target), Origin::Key));
            target = wraps ? (target + count - 1) % count : target - 1;
        }
    }
    if (target < 0 || target >= int(list.size())) {
        earcons_.play(Earcon::Edge);
        say(target < 0 ? "First " + unit() + "." : "Last " + unit() + ".");
        return;
    }
    sayUnavailable(skipped);
    if (!gotoChapter(target, Origin::Key)) return; // the error was said
    earcons_.play(Earcon::Move);
    // The track that plays, which is a later one when the queue skipped entries that cannot play.
    say(list[filePerChapter() ? fileIndex_ : size_t(target)].title);
}

void App::onFileFinished() {
    scrobble_.reset(); // its play is over: opening it again (Play at the end of the list) is a new one
    if (std::optional<size_t> next = upcomingTrack()) {
        // Quietly: "Loading." between every two tracks of an album would be too much.
        // If a remote one fails in a book, "Server not reachable" is said and it stops there. The
        // finished track was heard to the end, so Play opens the one that failed. In a music queue the
        // server is marked down instead and the entries after it that can play go on (openFile()).
        const size_t target = *next;
        const bool remote = trackRemote(target);
        auto failedHere = [this, target, remote] {
            if (!book_ || !remote) return;
            setFile(target);
            reopenAt_ = 0.0;
        };
        OpenResult result = loadFile(target, 0, true, Origin::Next, {}, failedHere);
        if (result != OpenResult::Failed || (remote && !music_)) return;
    }
    recordProgress(true);
    player_.pause();
    // The first entry waits closed when it is a server track, or a queue entry that cannot play now.
    if (trackRemote(0) || (music_ && trackCount() > 0 && availability(0, Origin::Next) != Availability::Yes)) {
        player_.close(); // next play starts the list again, opening it only then
        setFile(0);
        reopenAt_ = 0.0;
    } else {
        loadFile(0, 0, false, Origin::Next); // next play starts the book again
    }
    rememberPosition();
    pushProgress();
    earcons_.play(Earcon::Close);
    say(music_ ? "End of queue. Press play to start again." : "End of book. Press play to start again.");
}

// What was buffered has been heard. Moving on to the next track would only wait
// on the same server, so this track stays current and Play opens it again here.
void App::onStreamFailed() {
    double at = player_.position();
    player_.close();
    reopenAt_ = at;
    if (music_)
        if (const std::optional<size_t> link = trackLink(fileIndex_)) markServerDown(*link);
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
    const std::optional<size_t> upcoming = upcomingTrack();
    bool wanted = !seeking_ && !seekAtEnd_ && player_.playing() && book_ && upcoming && trackRemote(*upcoming) && player_.duration() > 0 &&
                  (!music_ || availability(*upcoming, Origin::Next) == Availability::Yes) &&
                  player_.duration() - player_.position() <= std::chrono::duration<double>(kReadAhead).count();
    if (wanted) {
        // A new track, or the quality changed since: pre-open the stream that will be opened.
        size_t next = *upcoming;
        const std::string id = trackKey(next);
        const size_t link = trackLink(next).value_or(0);
        auto cached = cachedFile(link, id);
        if (cached && !upgradeCached(link, *cached, streamingBitrate())) { // a sufficient cached file needs no preload
            if (readAhead_) {
                player_.cancelPreload();
                readAhead_.reset();
            }
            return;
        }
        if (readAhead_ && readAhead_->link == link && readAhead_->trackId == id && readAhead_->bitrate == streamingBitrate()) return;
        readAhead_ = ReadAhead{link, id, streamingBitrate(), newStream(next)};
        player_.preload(readAhead_->stream.url);
    } else if (readAhead_) {
        player_.cancelPreload();
        readAhead_.reset();
    }
}

void App::startTrackPlay() {
    scrobble_ = TrackPlay{};
    scrobble_->link = trackLink(fileIndex_).value_or(0);
    scrobble_->trackId = trackKey(fileIndex_);
    scrobble_->startedAt = clock_.wallTime();
    // A cached track that plays is kept longer: the time goes to its manifest with the next trimCache().
    // (trimCache() ignores a time before kEarliestPlayTime: a clock that was not set.)
    if (cachedFile(scrobble_->link, scrobble_->trackId)) linkState(scrobble_->link).played[scrobble_->trackId] = scrobble_->startedAt;
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
        serverAt(play.link).scrobble(play.trackId, false, std::nullopt, {}); // "now playing" is never kept
    }
    if (play.heardSince) play.heard += now - *play.heardSince;
    play.heardSince = now;
    // The player knows the length once open; a transcode has the server's (or none).
    double length = player_.duration() > 0 ? player_.duration() : trackDuration(fileIndex_);
    Clock::TimePoint::duration enough = kScrobbleAtMost;
    if (length > 0)
        enough = std::min(enough, std::chrono::duration_cast<Clock::TimePoint::duration>(
                                      std::chrono::duration<double>(length * kScrobbleShare)));
    if (!play.submitted && play.heard >= enough) {
        play.submitted = true;
        submitScrobble(play.link, play.trackId);
    }
}

// The complete downloaded file of remote track `id`, if the cache is set up for the
// server in the config and has one.
std::optional<std::filesystem::path> App::cachedFile(size_t link, const std::string& id, bool verify) const {
    if (cacheRoot_.empty()) return std::nullopt;
    const TrackCache& store = trackCacheOf(link);
    if (!store.enabled()) return std::nullopt;
    LinkState& state = linkState(link);
    auto& cacheIndex = state.cacheIndex;
    if (!cacheIndex) cacheIndex = store.index(&state.cacheQualities);
    auto it = cacheIndex->find(id);
    if (it == cacheIndex->end()) return std::nullopt;
    if (verify) {
        std::error_code ec;
        auto size = std::filesystem::file_size(it->second, ec);
        if (ec || size == 0) { // gone or emptied since the index was made
            cacheIndex->erase(it);
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

// The servers in the config are set up before the player starts and no menu changes them (a factory
// reset keeps them), so each cache is made once. With no server the cache has an empty url: it stores nothing.
const TrackCache& App::trackCacheOf(size_t link) const {
    static NoTextFiles none;
    std::optional<TrackCache>& cache = linkState(link).cache;
    if (!cache) {
        const ServerConfig config = link < servers_.size() ? servers_[link].config : ServerConfig{};
        cache.emplace(textFiles_ ? *textFiles_ : none, textFiles_ ? cacheRoot_ : std::filesystem::path(), config.url, config.user);
    }
    return *cache;
}

// ---------------------------------------------------------------- downloads

bool App::canDownloadFrom(size_t link) const {
    return link < servers_.size() && servers_[link].downloader.configured() && trackCacheOf(link).enabled(); // no cache folder: not enabled
}

std::string App::downloadProgress() const {
    if (!download_) return "";
    return "Downloading, " + std::to_string(download_->finished) + " of " + countValue(download_->total, download_->unit);
}

namespace {
// The track's file in its list's folder: its id, which is unique, and its type.
std::string downloadFileName(const RemoteTrack& track, int bitrate = 0) {
    std::string name = TrackCache::safeName(track.id);
    // safeName escapes ~ in both ids and suffixes, so this cannot alias an original.
    if (bitrate > 0) name += "~" + std::to_string(bitrate) + ".mp3";
    else if (!track.suffix.empty()) name += "." + TrackCache::safeName(track.suffix);
    return name;
}

std::uintmax_t downloadBytes(const RemoteTrack& track, int bitrate) {
    long double bytes = bitrate > 0 && std::isfinite(track.duration) && track.duration > 0
                        ? std::ceil(static_cast<long double>(track.duration) * bitrate * 1000 / 8)
                        : std::max(0LL, track.size);
    const auto maximum = std::numeric_limits<std::uintmax_t>::max();
    return bytes >= static_cast<long double>(maximum) ? maximum : std::uintmax_t(bytes);
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
    for (CachedTrack t : before) {
        if (listed(t.id)) continue;
        if (cached.source.kind == RemoteSource::Kind::KeptPlaylist) {
            t.pinned = false;
            extra.push_back(std::move(t));
        } else (t.pinned ? dropped : extra).push_back(std::move(t));
    }
    kept.insert(kept.end(), extra.begin(), extra.end());
    bool changed = kept.size() != before.size();
    for (size_t i = 0; !changed && i < kept.size(); ++i) changed = kept[i].id != before[i].id;
    cached.tracks = std::move(kept);
    return {changed, std::move(dropped)};
}
} // namespace

// Sets `changed` of every track of the manifest; true if one was different.
bool App::markTracks(CachedList& cached, const char* mark) {
    bool changed = false;
    for (CachedTrack& t : cached.tracks) {
        changed = changed || t.changed != mark;
        t.changed = mark;
    }
    return changed;
}

void App::deleteDropped(const RemoteSource& source, const std::vector<CachedTrack>& dropped, size_t link) {
    const TrackCache& store = trackCacheOf(link);
    for (const CachedTrack& t : dropped) {
        std::error_code ec;
        std::filesystem::path file = store.listFolder(source) / t.file;
        std::filesystem::remove(file, ec);
        forgetCached(t.id, file, link);
    }
}

std::string App::trackDestination(const RemoteSource& source, const RemoteTrack& track, size_t link, int bitrate) const {
    return (trackCacheOf(link).listFolder(source) / downloadFileName(track, bitrate)).string();
}

// The index has one file per track id, and a track can be in two lists (an album and a playlist).
void App::forgetCached(const std::string& id, const std::filesystem::path& file, size_t link) {
    auto& cacheIndex = linkState(link).cacheIndex;
    if (!cacheIndex) return;
    auto it = cacheIndex->find(id);
    if (it == cacheIndex->end() || it->second != file) return;
    cacheIndex->erase(it);
    if (auto other = trackCacheOf(link).find(id)) (*cacheIndex)[id] = *other;
}

// A fetched track goes into its list's manifest: the entry is added or replaced, keeping what the
// manifest knows of it (a pinned flag stays; lastPlayed stays unless `playedAt` is a real time, then
// the latest play wins). `pinned`: a download the user asked for. `order`, for a download: the
// list as the server has it, which the manifest then follows (the pinned tracks it drops are
// deleted). `replaceName`: the list is called `title` even if the manifest has another name, else
// a list that has a name keeps it (a list the user downloaded). False if the manifest cannot be written.
bool App::addToManifest(const RemoteSource& source, const RemoteTrack& track, const ManifestUpdate& update, size_t link) {
    const TrackCache& store = trackCacheOf(link);
    if (!store.enabled()) return false;
    CachedList cached = store.load(source);
    cached.source = source;
    if (update.replaceName || cached.name.empty()) {
        cached.name = update.title;
        cached.artist = update.artist;
    }
    if (update.chapters) cached.chapters = *update.chapters;
    std::string fileName = downloadFileName(track, update.bitrate);
    CachedTrack entry{track.id, fileName, track.title, track.duration, update.order ? kPartialMark : "", update.pinned,
                      update.playedAt >= kEarliestPlayTime ? update.playedAt : 0, update.bitrate};
    std::vector<CachedTrack> dropped;
    auto old = std::find_if(cached.tracks.begin(), cached.tracks.end(), [&](const CachedTrack& t) { return t.id == track.id; });
    if (old != cached.tracks.end()) {
        entry.pinned = entry.pinned || old->pinned;
        if (!update.order) entry.changed = old->changed; // a play or the automatic cache does not end a download
        if (entry.lastPlayed == 0) entry.lastPlayed = old->lastPlayed;
        if (old->file != fileName) dropped.push_back(*old); // retire the lower-quality file only after the manifest commits
        *old = entry;
    } else {
        cached.tracks.push_back(entry); // appended: a download's manifest then follows the order below
    }
    if (update.order && source.kind != RemoteSource::Kind::KeptPlaylist) {
        auto removed = followServerOrder(cached, *update.order).second;
        dropped.insert(dropped.end(), removed.begin(), removed.end());
    }
    if (!store.save(cached)) return false;
    for (const auto& prior : dropped)
        if (prior.id == track.id && prior.file != fileName)
            replaceCachedPath(store.listFolder(source) / prior.file, store.listFolder(source) / fileName);
    deleteDropped(source, dropped, link);
    if (auto& cacheIndex = linkState(link).cacheIndex) {
        const auto path = store.listFolder(source) / fileName;
        (*cacheIndex)[track.id] = path;
        linkState(link).cacheQualities[path] = update.bitrate;
    }
    return true;
}

void App::replaceCachedPath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    const std::string old = oldPath.string(), replacement = newPath.string();
    for (auto& [from, to] : replacedCacheFiles_)
        if (to == old) to = replacement;
    replacedCacheFiles_[old] = replacement;
    bool changed = queue_.replaceLocalPath(old, replacement);
    if (parked_ && parked_->music) changed = parked_->queue.replaceLocalPath(old, replacement) || changed;
    if (previous_) previous_->queue.replaceLocalPath(old, replacement);
    if (changed) saveQueue(); // a saved local queue must not keep the file we are about to retire
}

// A download runs: for the same list it says how far it is, for another it refuses.
bool App::downloading(const RemoteSource& source, size_t link) const {
    return download_ && download_->link == link && sameList(download_->source, source);
}

void App::sayDownloadBusy(const RemoteSource& source, size_t link) {
    say(downloading(source, link) ? downloadProgress() + "." : "Another download is running.");
}

std::string App::keptPlaylistKey(size_t link, const std::string& id) const {
    return "playlist.keep:" + TrackCache::safeName(servers_[link].key) + ":" + TrackCache::safeName(id);
}

std::string App::keptRevisionKey(size_t link, const std::string& id) const {
    return "playlist.keep.revision:" + TrackCache::safeName(servers_[link].key) + ":" + TrackCache::safeName(id);
}

std::string App::keptReadyKey(size_t link, const std::string& id) const {
    return "playlist.keep.ready:" + TrackCache::safeName(servers_[link].key) + ":" + TrackCache::safeName(id);
}

bool App::setPlaylistKept(size_t link, const std::string& id, const std::string& title, bool keep) {
    const auto key = keptPlaylistKey(link, id);
    if (keep) {
        ++keptGenerations_[key];
        state_.setInt(key, 1);
        state_.erase(keptReadyKey(link, id));
        keptJobs_.push_back({link, id, title, {}, false, screens_.empty() ? 0 : screens_.back().id});
        return true;
    }
    const RemoteSource source{RemoteSource::Kind::KeptPlaylist, id};
    const auto& store = trackCacheOf(link);
    auto cached = store.load(source);
    bool changed = false;
    for (auto& track : cached.tracks) if (track.pinned) { track.pinned = false; changed = true; }
    if (changed && !store.save(cached)) return false;
    ++keptGenerations_[key];
    state_.erase(key);
    if (keptRequest_ && keptRequestLink_ == link && keptRequestPlaylist_ == id) {
        serverAt(link).cancel(std::exchange(keptRequest_, 0));
        keptRequestPlaylist_.clear();
    }
    keptJobs_.erase(std::remove_if(keptJobs_.begin(), keptJobs_.end(), [&](const KeptJob& job) { return job.link == link && job.id == id; }), keptJobs_.end());
    if (downloading(source, link)) {
        servers_[link].downloader.cancel(download_->request);
        download_.reset();
    }
    return true;
}

void App::scheduleKeptPlaylist(size_t link, const std::string& id, const std::string& title) {
    auto found = std::find_if(keptJobs_.begin(), keptJobs_.end(), [&](const KeptJob& job) { return job.link == link && job.id == id; });
    if (found == keptJobs_.end()) keptJobs_.push_back({link, id, title, {}, false, refresh_.active ? refresh_.screen : 0});
}

void App::pumpKeptPlaylists() {
    if (keptRequest_ || download_ || keptJobs_.empty()) return;
    KeptJob job = keptJobs_.front();
    keptJobs_.erase(keptJobs_.begin());
    if (!state_.getInt(keptPlaylistKey(job.link, job.id), 0)) return;
    if (!job.fetched) {
        keptRequestLink_ = job.link;
        keptRequestPlaylist_ = job.id;
        job.generation = keptGenerations_[keptPlaylistKey(job.link, job.id)];
        job.version = state_.getInt(keptRevisionKey(job.link, job.id), 0);
        keptRequest_ = serverAt(job.link).playlistTracks(job.id, [this, job](MusicServer::Error error, std::vector<RemoteTrack> tracks, bool) mutable {
            keptRequest_ = 0;
            keptRequestPlaylist_.clear();
            const auto key = keptPlaylistKey(job.link, job.id);
            if (!state_.getInt(key, 0) || keptGenerations_[key] != job.generation) return;
            if (error != MusicServer::Error::None) {
                if (job.screen && !sleeping_ && !screens_.empty() && screens_.back().id == job.screen) {
                    earcons_.play(Earcon::Error);
                    say(errorText(error) + ".");
                }
                return;
            }
            if (job.version != state_.getInt(keptRevisionKey(job.link, job.id), 0)) {
                scheduleKeptPlaylist(job.link, job.id, job.title);
                return;
            }
            job.tracks = std::move(tracks);
            job.fetched = true;
            keptJobs_.push_back(std::move(job));
        });
        return;
    }
    if (job.version != state_.getInt(keptRevisionKey(job.link, job.id), 0)) {
        scheduleKeptPlaylist(job.link, job.id, job.title);
        return;
    }
    const bool visible = job.screen && !sleeping_ && !screens_.empty() && screens_.back().id == job.screen;
    startDownload(job.link, job.title, "", job.tracks, {RemoteSource::Kind::KeptPlaylist, job.id}, {}, "track", {}, !visible);
}

// OK on a Download item. While that list downloads it says how far it is. A
// download that failed has been dropped, so OK goes on with what is missing:
// tracks already whole are skipped and the .part of the failed one continues.
void App::startDownload(size_t link, const std::string& title, const std::string& artist, const std::vector<RemoteTrack>& tracks,
                        const RemoteSource& source, const std::function<void()>& started, const char* unit,
                        const std::vector<RemoteChapter>& chapters, bool background) {
    if (download_) {
        sayDownloadBusy(source, link);
        return;
    }
    const TrackCache& store = trackCacheOf(link);
    if (!store.enabled()) return;
    CachedList cached = store.load(source);
    std::vector<CachedTrack>& manifest = cached.tracks;
    Download job;
    job.background = background;
    job.bitrate = serverAt(link).catalogue() == MusicServer::Catalogue::Books ? 0 :
                  std::max(0, config_.getInt(kDownloadBitrate, kOriginalQuality));
    job.link = link;
    job.source = source;
    job.title = title;
    job.artist = artist;
    job.unit = unit;
    job.chapters = chapters;
    bool pinned = false; // a track the automatic cache kept now belongs to the user
    std::uintmax_t needed = 0;
    std::map<std::string, std::pair<std::filesystem::path, int>> reusable;
    std::vector<std::filesystem::path> newLinks;
    std::vector<std::pair<CachedTrack, std::filesystem::path>> replacedLinks;
    for (const auto& list : store.lists()) {
        if (source.kind != RemoteSource::Kind::KeptPlaylist && list.source.kind != RemoteSource::Kind::KeptPlaylist) continue;
        for (const auto& t : list.tracks) {
            if (!qualityAtLeast(t.bitrate, job.bitrate)) continue;
            if (auto file = store.completeFile(list.source, t)) reusable.try_emplace(t.id, *file, t.bitrate);
        }
    }
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (source.kind == RemoteSource::Kind::KeptPlaylist) job.fullOrder.push_back(tracks[i].id);
        // A track listed twice is one file: fetched and counted once.
        if (std::find(job.order.begin(), job.order.end(), tracks[i].id) != job.order.end()) continue;
        job.order.push_back(tracks[i].id);
        auto entry = std::find_if(manifest.begin(), manifest.end(),
                                  [&](const CachedTrack& t) { return t.id == tracks[i].id; });
        const auto existing = entry != manifest.end() ? store.completeFile(source, *entry) : std::nullopt;
        if (existing) {
            // The requested download owns even a lower copy while its replacement arrives.
            if (!entry->pinned) entry->pinned = pinned = true;
            if (qualityAtLeast(entry->bitrate, job.bitrate)) {
                job.finished++;
                continue;
            }
        }
        if (reusable.contains(tracks[i].id)) {
            const auto& [file, bitrate] = reusable.at(tracks[i].id);
            const auto dest = store.listFolder(source) / downloadFileName(tracks[i], bitrate);
            std::error_code ec;
            std::filesystem::create_directories(store.listFolder(source), ec);
            if (!ec) std::filesystem::create_hard_link(file, dest, ec);
            if (ec) {
                for (const auto& path : newLinks) { std::error_code ignored; std::filesystem::remove(path, ignored); }
                if (!background) { earcons_.play(Earcon::Error); say("Cannot write the download."); }
                return;
            }
            newLinks.push_back(dest);
            CachedTrack shared{tracks[i].id, dest.filename().string(), tracks[i].title, tracks[i].duration,
                               kPartialMark, true, entry == manifest.end() ? 0 : entry->lastPlayed, bitrate};
            if (entry == manifest.end()) manifest.push_back(std::move(shared));
            else {
                if (existing && *existing != dest) replacedLinks.emplace_back(*entry, dest);
                *entry = std::move(shared);
            }
            pinned = true;
            ++job.finished;
            continue;
        }
        job.todo.push_back({tracks[i], i + 1});
        const auto bytes = downloadBytes(tracks[i], job.bitrate);
        const auto maximum = std::numeric_limits<std::uintmax_t>::max();
        needed = bytes >= maximum - needed ? maximum : needed + bytes;
    }
    job.total = job.order.size();
    if (source.kind == RemoteSource::Kind::Playlist || source.kind == RemoteSource::Kind::KeptPlaylist) {
        job.playlistVersion = source.kind == RemoteSource::Kind::KeptPlaylist ? state_.getInt(keptRevisionKey(link, source.id), 0)
            : state_.getInt(stalePlaylistKey({link, source.id, title, 0, false}), 0);
        job.playlistUnique = source.kind == RemoteSource::Kind::KeptPlaylist || job.order.size() == tracks.size();
    }
    cached.source = source;
    cached.name = title;
    cached.artist = artist;
    bool reordered = false;
    std::vector<CachedTrack> dropped;
    if (source.kind != RemoteSource::Kind::KeptPlaylist || job.todo.empty()) {
        if (source.kind == RemoteSource::Kind::KeptPlaylist) cached.order = job.fullOrder;
        auto result = followServerOrder(cached, job.order);
        reordered = result.first || source.kind == RemoteSource::Kind::KeptPlaylist;
        dropped = std::move(result.second);
    }
    // Nothing to fetch: what is here is the whole list, whatever an earlier try left marked.
    if (job.todo.empty()) pinned = markTracks(cached, "") || pinned;
    if ((pinned || reordered || !dropped.empty()) && !store.save(cached)) {
        for (const auto& path : newLinks) { std::error_code ignored; std::filesystem::remove(path, ignored); }
        if (!background) { earcons_.play(Earcon::Error); say("Cannot write the download."); }
        return;
    }
    for (const auto& [old, replacement] : replacedLinks) {
        replaceCachedPath(store.listFolder(source) / old.file, replacement);
        deleteDropped(source, {old}, link);
    }
    deleteDropped(source, dropped, link);
    if (job.todo.empty()) {
        if (source.kind == RemoteSource::Kind::KeptPlaylist && state_.getInt(keptRevisionKey(link, source.id), 0) == job.playlistVersion)
            state_.setInt(keptReadyKey(link, source.id), job.playlistVersion);
        else if (source.kind == RemoteSource::Kind::Playlist && job.playlistUnique &&
            state_.getInt(stalePlaylistKey({link, source.id, title, 0, false}), 0) == job.playlistVersion)
            state_.erase(stalePlaylistKey({link, source.id, title, 0, false}));
        if (!background) say("Already downloaded.");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(cacheRoot_, ec);
    std::optional<std::uintmax_t> room = freeSpace_(cacheRoot_);
    if (needed > 0 && room && *room < needed) {
        if (!background) { earcons_.play(Earcon::Error); say("Not enough space."); }
        return;
    }
    // From here on the manifest says the list is not whole until the last track is in.
    if (source.kind != RemoteSource::Kind::KeptPlaylist && markTracks(cached, kPartialMark) && !store.save(cached)) {
        if (!background) { earcons_.play(Earcon::Error); say("Cannot write the download."); }
        return;
    }
    pauseAutoCache(); // an explicit download goes first
    download_ = std::move(job);
    if (started) started(); // before the words: the screen it leaves would speak over them
    if (!background) say(downloadProgress() + ".");
    fetchNextTrack();
}

void App::fetchNextTrack() {
    Download& job = *download_;
    const RemoteTrack& track = job.todo[job.next].track;
    job.request = servers_[job.link].downloader.download(track.id, trackDestination(job.source, track, job.link, job.bitrate), track.size, {}, [this](Downloader::Result result) { onTrackDownloaded(result); }, job.bitrate);
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
    if (!addToManifest(job.source, track, {job.title, job.artist, true, true, 0, &job.order, &job.chapters, job.bitrate}, job.link)) {
        std::error_code ec; // the whole file is in place but unlisted: nothing would count or evict it
        std::filesystem::remove(trackDestination(job.source, track, job.link, job.bitrate), ec);
        failDownload(track, item.number, "Cannot write the file");
        return;
    }
    job.finished++;
    if (++job.next < job.todo.size()) fetchNextTrack();
    else finishDownload();
}

// Spoken once; the download is dropped. OK on Download again tries what is missing.
void App::failDownload(const RemoteTrack& track, size_t number, const std::string& reason) {
    std::string unit = "track"; // a file of a book has no title worth speaking: it is "file 3"
    if (download_) {
        failedPart_ = trackDestination(download_->source, track, download_->link, download_->bitrate) + ".part";
        unit = download_->unit;
    }
    const bool background = download_ && download_->background;
    download_.reset();
    if (background) return;
    if (sleeping_) return;
    earcons_.play(Earcon::Error);
    std::string name = track.title.empty() ? unit + " " + std::to_string(number) : track.title;
    say("Cannot download " + name + ". " + reason + ".");
}

// Over something that plays an earcon is enough; otherwise it is said.
void App::finishDownload() {
    std::string title = download_->title;
    const bool background = download_->background;
    const TrackCache& store = trackCacheOf(download_->link);
    CachedList cached = store.load(download_->source);
    bool changed = markTracks(cached, "");
    if (download_->source.kind == RemoteSource::Kind::KeptPlaylist &&
        state_.getInt(keptRevisionKey(download_->link, download_->source.id), 0) == download_->playlistVersion) {
        cached.order = download_->fullOrder;
        followServerOrder(cached, download_->order);
        changed = true;
    }
    const bool saved = !changed || store.save(cached); // if unsaved, the list stays partial
    if (saved && download_->source.kind == RemoteSource::Kind::KeptPlaylist &&
        state_.getInt(keptRevisionKey(download_->link, download_->source.id), 0) == download_->playlistVersion)
        state_.setInt(keptReadyKey(download_->link, download_->source.id), download_->playlistVersion);
    else if (saved && download_->source.kind == RemoteSource::Kind::Playlist && download_->playlistUnique) {
        PlaylistEntry p{download_->link, download_->source.id, title, 0, false};
        if (state_.getInt(stalePlaylistKey(p), 0) == download_->playlistVersion) state_.erase(stalePlaylistKey(p));
    }
    download_.reset();
    if (background) return;
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
    // A server book is not cached on its own: its tracks would be listed in Music as an album and
    // would push the music out of the limit (downloading books for offline is 4.12).
    if (!book_ || !book_->remote || book_->serverBook || !book_->source || !trackRemote(fileIndex_) ||
        trackKey(fileIndex_) != scrobble_->trackId)
        return;
    // Only a track of the list the queue came from: its details and its folder are that list's.
    // A queue can mix servers, and ids are only unique on one: the track must be of the server the list is on.
    if (scrobble_->link != book_->link) return;
    if (!openedFromStream_) return;
    const std::optional<size_t> inList = listIndexOf(scrobble_->trackId, fileIndex_);
    if (!inList) return;
    std::uintmax_t limit = cacheLimit();
    if (limit == 0) return; // (pumpAutoCache() skips what is cached, and asks about the downloader)
    AutoCache job;
    job.source = *book_->source;
    job.title = book_->title;
    job.artist = book_->artist;
    job.playedAt = scrobble_->startedAt;
    job.link = scrobble_->link;
    job.bitrate = openedStreamBitrate_;
    job.track.id = scrobble_->trackId;
    if (*inList < book_->titles.size()) job.track.title = book_->titles[*inList];
    if (*inList < book_->durations.size()) job.track.duration = book_->durations[*inList];
    if (*inList < book_->suffixes.size()) job.track.suffix = book_->suffixes[*inList];
    if (*inList < book_->sizes.size()) job.track.size = book_->sizes[*inList];
    if (downloadBytes(job.track, job.bitrate) > limit) return; // would be evicted at once: fetched at every play
    autoQueue_.push_back(std::move(job));
}

void App::pumpAutoCache() {
    if (autoRequest_ || download_ || sleeping_ || autoQueue_.empty()) return;
    // Switched off since, or the job's server cannot download (no cache folder): the jobs are dropped.
    const size_t jobs = autoQueue_.size();
    if (cacheLimit() == 0) autoQueue_.clear();
    else std::erase_if(autoQueue_, [&](const AutoCache& job) { return !canDownloadFrom(job.link); });
    if (autoQueue_.size() != jobs) trimDirty_ = true; // a .part of the dropped jobs may be left
    if (autoQueue_.empty()) return;
    // A download has fetched it meanwhile.
    while (!autoQueue_.empty()) {
        const auto& job = autoQueue_.front();
        auto cached = cachedFile(job.link, job.track.id, true);
        if (!cached || upgradeCached(job.link, *cached, job.bitrate)) break;
        autoQueue_.pop_front();
    }
    if (autoQueue_.empty()) return;
    // Never fill the card: a track that leaves less than the reserve is skipped, silently, and
    // nothing else starts in this tick (the next one looks at the next track).
    std::error_code ec;
    std::filesystem::create_directories(cacheRoot_, ec);
    std::optional<std::uintmax_t> room = freeSpace_(cacheRoot_);
    std::uintmax_t size = downloadBytes(autoQueue_.front().track, autoQueue_.front().bitrate);
    if (room && (*room < kAutoCacheReserve || size > *room - kAutoCacheReserve)) {
        autoQueue_.pop_front();
        return;
    }
    autoJob_ = std::move(autoQueue_.front());
    autoQueue_.pop_front();
    autoRequest_ = servers_[autoJob_->link].downloader.download(autoJob_->track.id, trackDestination(autoJob_->source, autoJob_->track, autoJob_->link, autoJob_->bitrate),
                                        autoJob_->track.size, {}, [this](Downloader::Result r) { onAutoCached(r); }, autoJob_->bitrate);
}

// The transfer in flight is cancelled on the downloader it was started on.
void App::cancelAutoRequest() {
    servers_[autoJob_->link].downloader.cancel(std::exchange(autoRequest_, 0));
}

// The transfer in flight stops (its .part stays) and is the next one when the download is over
// or the device wakes.
void App::pauseAutoCache() {
    if (!autoRequest_) return;
    cancelAutoRequest();
    autoQueue_.push_front(std::move(*autoJob_));
    autoJob_.reset();
}

void App::dropAutoCache(const RemoteSource& list, size_t link) {
    // The jobs of that server's cache folder: another server may have a list with the same id.
    std::erase_if(autoQueue_, [&](const AutoCache& job) { return job.link == link && sameList(job.source, list); });
    if (autoRequest_ && autoJob_->link == link && sameList(autoJob_->source, list)) {
        cancelAutoRequest();
        autoJob_.reset();
        trimDirty_ = true; // its .part is abandoned
    }
}

// Silent, whatever happens. A failure leaves its .part: the next try resumes an
// original, while a transcode restarts it.
void App::onAutoCached(Downloader::Result result) {
    autoRequest_ = 0;
    if (!autoJob_) return;
    AutoCache job = std::move(*autoJob_);
    autoJob_.reset();
    if (result != Downloader::Result::Ok) {
        trimDirty_ = true; // the .part is abandoned unless the track is heard again
        return;
    }
    const TrackCache& store = trackCacheOf(job.link);
    if (!store.enabled()) return;
    std::filesystem::path file = store.listFolder(job.source) / downloadFileName(job.track, job.bitrate);
    if (!addToManifest(job.source, job.track, {job.title, job.artist, false, false, job.playedAt, nullptr, nullptr, job.bitrate}, job.link)) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
        return;
    }
    if (auto& cacheBytes = linkState(job.link).cacheBytes) {
        std::error_code ec;
        auto size = std::filesystem::file_size(file, ec);
        if (!ec) *cacheBytes += size;
    }
    trimCache(false);
}

// The .part files that belong to a transfer that runs or waits: a trim must not touch them.
std::vector<std::filesystem::path> App::partsInUse() const {
    std::vector<std::filesystem::path> parts;
    if (!failedPart_.empty()) parts.push_back(failedPart_);
    auto add = [&](const RemoteSource& source, const RemoteTrack& track, size_t link, int bitrate = 0) {
        parts.push_back(trackDestination(source, track, link, bitrate) + ".part");
    };
    if (download_)
        for (size_t i = download_->next; i < download_->todo.size(); ++i) add(download_->source, download_->todo[i].track, download_->link, download_->bitrate);
    if (autoJob_) add(autoJob_->source, autoJob_->track, autoJob_->link, autoJob_->bitrate);
    for (const AutoCache& job : autoQueue_) add(job.source, job.track, job.link, job.bitrate);
    return parts;
}

// Evicts what is over the limit; with `flushPlays`, also writes the plays recorded in memory
// into the manifests. Scans the manifests once, and only if there is a reason: the plays to write,
// or the unpinned bytes (counted at the last scan, plus the tracks cached since) are or may be
// over the limit. Called when a track was cached and on exit and sleep, never from a key or a tick.
// A limit of 0 (off) evicts nothing: what is cached stays, only nothing new is added.
// Each server's cache is trimmed on its own, to the same limit.
void App::trimCache(bool flushPlays) {
    if (cacheRoot_.empty()) return;
    const std::uintmax_t limit = cacheLimit();
    std::optional<std::vector<std::filesystem::path>> parts; // worked out when a server needs a trim
    bool dirty = false;
    for (size_t link = 0; link < servers_.size(); ++link) {
        const TrackCache& store = trackCacheOf(link);
        if (!store.enabled()) continue;
        LinkState& state = linkState(link);
        bool over = limit > 0 && (trimDirty_ || !state.cacheBytes || *state.cacheBytes > limit);
        if (!over && !(flushPlays && !state.played.empty())) continue;
        if (!parts) parts = partsInUse();
        std::vector<std::filesystem::path> keep = *parts; // and the file the player has open
        if (book_ && trackRemote(fileIndex_) && player_.isOpen() && trackLink(fileIndex_) == std::optional<size_t>(link))
            if (auto open = cachedFile(link, trackKey(fileIndex_))) keep.push_back(*open);
        TrackCache::TrimInfo info;
        store.trim(limit > 0 ? limit : std::numeric_limits<std::uintmax_t>::max(), state.played, keep, &info);
        state.played.clear();
        dirty = true;
        state.cacheBytes = info.remaining;
        if (state.cacheIndex)
            for (const auto& [id, file] : info.evicted) forgetCached(id, file, link);
    }
    if (dirty) trimDirty_ = false;
}

bool App::upgradeCached(size_t link, const std::filesystem::path& file, int bitrate) const {
    if (config_.getInt(kCacheUpgrade, 0) == 0 || linkState(link).down) return false;
    const auto& qualities = linkState(link).cacheQualities;
    auto found = qualities.find(file);
    return found != qualities.end() && !qualityAtLeast(found->second, bitrate);
}

bool App::openCachedFallback(size_t file, double position, bool autoplay, Origin origin, const Then& opened) {
    const size_t link = trackLink(file).value_or(0);
    markServerDown(link);
    auto cached = cachedFile(link, trackKey(file), true);
    if (!cached || !player_.open(cached->string(), position)) return false;
    openedFromStream_ = false;
    onOpened(autoplay, origin, opened);
    return true;
}

int App::streamingBitrate() const { return std::max(kOriginalQuality, config_.getInt(kStreamingBitrate, kOriginalQuality)); }

MusicServer::Stream App::remoteStream(size_t file) {
    const std::string id = trackKey(file);
    const size_t link = trackLink(file).value_or(0);
    if (readAhead_ && readAhead_->link == link && readAhead_->trackId == id && readAhead_->bitrate == streamingBitrate())
        return readAhead_->stream;
    return newStream(file);
}

MusicServer::Stream App::newStream(size_t file) {
    MusicServer::Stream stream = serverAt(trackLink(file).value_or(0)).stream(trackKey(file), streamingBitrate());
    // A transcode often has no length of its own: the server's listing has it.
    if (stream.transcoded && trackDuration(file) > 0) stream.transcoded->duration = trackDuration(file);
    return stream;
}

// A pull belongs to this opening of the book, not to the server's current screen.
void App::cancelProgressPull() {
    ++progressPullGeneration_;
    if (progressPull_) serverAt(progressPullLink_).cancel(std::exchange(progressPull_, 0));
    progressPullScreen_ = 0;
    serverPlace_.reset();
}

std::optional<App::ServerPlace> App::serverPlace(RemoteProgress progress) const {
    if (!book_ || book_->files.empty() || !std::isfinite(progress.seconds) || progress.seconds < 0) return std::nullopt;
    ServerPlace place;
    place.progress = progress;
    place.position = progress.seconds;
    while (place.file + 1 < book_->files.size()) {
        const double length = trackDuration(place.file);
        if (length <= 0) return std::nullopt; // no timeline across files of unknown length
        if (place.position < length) break;
        place.position -= length;
        ++place.file;
    }
    const double length = trackDuration(place.file);
    if (length > 0) place.position = std::min(place.position, length);
    const auto list = chapters();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].file < place.file || (list[i].file == place.file && list[i].start <= place.position + kChapterPositionSlack))
            place.chapter = int(i);
    return place;
}

void App::pullProgress(bool hadLocal, std::string localStamp, bool followServer) {
    cancelProgressPull();
    if (!book_ || music_ || !book_->serverBook || !book_->source || book_->link >= servers_.size() ||
        !serverAt(book_->link).syncsProgress()) return;
    const auto screen = playerScreenIndex();
    if (!screen) return;
    progressPullScreen_ = screens_[*screen].id;
    progressPullLink_ = book_->link;
    const auto generation = progressPullGeneration_;
    const std::string id = book_->path.string();
    const std::weak_ptr<int> lifetime = progressLifetime_;
    progressPull_ = serverAt(book_->link).progress(book_->source->id,
        [this, lifetime, generation, id, hadLocal, followServer, localStamp = std::move(localStamp)](MusicServer::Error error,
                                                                                  std::optional<RemoteProgress> progress) {
            if (lifetime.expired() || generation != progressPullGeneration_ || shutDown_ || sleeping_ || quit_ ||
                !book_ || book_->path.string() != id) return;
            progressPull_ = 0;
            if (error != MusicServer::Error::None || !progress) return;
            if (followServer) {
                if (const auto target = serverPlace(*progress)) useServerPlace(*target);
                return;
            }
            const std::string currentStamp = state_.getString("posat:" + id);
            std::int64_t stamp = 0;
            try { if (!(currentStamp.empty() ? localStamp : currentStamp).empty())
                stamp = std::stoll(currentStamp.empty() ? localStamp : currentStamp); }
            catch (...) { return; }
            if (static_cast<long double>(progress->updated) <= static_cast<long double>(stamp) * kProgressMillisPerSecond ||
                std::abs(progress->seconds - bookTime(fileIndex_, place())) <= kServerPlaceDistance) return;
            const auto target = serverPlace(*progress);
            if (!target) return;
            if (!hadLocal && currentStamp.empty()) {
                useServerPlace(*target);
            } else {
                serverPlace_ = *target;
                say("The server has a later place, chapter " + std::to_string(target->chapter + 1) + ". It is in Options.");
            }
        });
}

void App::useServerPlace(ServerPlace target) {
    if (!book_ || music_) return;
    const std::string id = book_->path.string();
    const auto generation = progressPullGeneration_;
    const bool needsSave = serverPlace_.has_value() || progressOut_.contains(id);
    auto arrived = [this, target, id, generation, needsSave] {
        if (generation != progressPullGeneration_ || !book_ || book_->path.string() != id) return;
        serverPlace_.reset();
        progressQueued_.erase(id); // a queued older local place must not replace the accepted one
        const std::string stamp = std::to_string(target.progress.updated / kProgressMillisPerSecond);
        state_.setString("pos:" + id, std::to_string(fileIndex_) + ":" + tenths(place()));
        state_.setString("posat:" + id, stamp);
        state_.erase("finished:" + id);
        if (needsSave) {
            // A pause after the notice may already have sent the old local
            // place. Serialize the accepted place after that save as well.
            state_.setString("pushdirty:" + id, "1");
            pushProgress();
        } else {
            state_.setString("pushed:" + id, stamp);
            state_.erase("pushdirty:" + id);
        }
        progressLast_ = bookTime(fileIndex_, place());
        progressHeard_ = {};
        say(chapters()[size_t(currentChapter())].title + ".");
    };
    if (target.file != fileIndex_ || !player_.isOpen())
        loadFile(target.file, target.position, willPlay(), Origin::Key, std::move(arrived));
    else {
        seekAtEnd_ = false;
        player_.seek(target.position);
        arrived();
    }
}

// Record only in memory. Whole seconds remain wall-clock seconds, even if two places
// are recorded in the same second; invalidate the acknowledgement in that case.
void App::recordProgress(bool finished) {
    if (!book_ || music_ || !book_->serverBook) return;
    const std::string id = book_->path.string();
    const std::string position = std::to_string(fileIndex_) + ":" + tenths(place());
    const bool sync = book_->link < servers_.size() && serverAt(book_->link).syncsProgress();
    if (!sync) { state_.setString("pos:" + id, position); return; }
    const bool wasFinished = state_.getString("finished:" + id) == "1";
    // End-of-book resets the local resume place to the beginning. Keep its
    // completion until the user actually starts listening again.
    if (!finished && wasFinished && !player_.playing()) {
        state_.setString("pos:" + id, position);
        return;
    }
    if (state_.getString("pos:" + id) == position && wasFinished == finished &&
        !state_.getString("posat:" + id).empty()) return;
    state_.setString("pos:" + id, position);
    if (finished) state_.setString("finished:" + id, "1");
    else state_.erase("finished:" + id);
    const auto at = static_cast<std::int64_t>(clock_.wallTime());
    state_.setString("posat:" + id, std::to_string(at));
    // Keep pushed as the last successful wall timestamp. This extra bit tracks
    // a changed place in the same wall second, or after the wall clock went back.
    state_.setString("pushdirty:" + id, "1");
}

void App::pushProgress() {
    if (!book_ || music_ || !book_->serverBook || pendingOpen_ ||
        book_->link >= servers_.size() || !serverAt(book_->link).syncsProgress() || !book_->source) return;
    rememberPosition();
    const std::string id = book_->path.string();
    ProgressSave save;
    save.link = book_->link;
    save.album = book_->source->id;
    save.position = state_.getString("pos:" + id);
    save.stamp = state_.getString("posat:" + id);
    if (save.stamp.empty() || (save.stamp == state_.getString("pushed:" + id) && state_.getString("pushdirty:" + id) != "1")) return;
    save.progress.finished = state_.getString("finished:" + id) == "1";
    for (double duration : book_->durations) save.progress.duration += duration;
    save.progress.seconds = save.progress.finished ? save.progress.duration : bookTime(fileIndex_, place());
    save.progress.updated = static_cast<std::int64_t>(clock_.wallTime()) * kProgressMillisPerSecond;
    sendProgress(id, std::move(save));
}

void App::sendProgress(const std::string& id, ProgressSave save) {
    if (auto it = progressOut_.find(id); it != progressOut_.end()) {
        if (it->second.position != save.position || it->second.stamp != save.stamp ||
            it->second.progress.finished != save.progress.finished) progressQueued_[id] = std::move(save);
        else progressQueued_.erase(id); // returning to the in-flight place supersedes any queued detour
        return;
    }
    progressOut_[id] = save;
    const std::weak_ptr<int> lifetime = progressLifetime_;
    serverAt(save.link).saveProgress(save.album, save.progress, [this, lifetime, id, save](MusicServer::Error error) {
        if (lifetime.expired()) return;
        progressOut_.erase(id);
        const bool same = state_.getString("posat:" + id) == save.stamp &&
            state_.getString("pos:" + id) == save.position &&
            (state_.getString("finished:" + id) == "1") == save.progress.finished;
        if (error == MusicServer::Error::None && same) {
            state_.setString("pushed:" + id, save.stamp);
            state_.erase("pushdirty:" + id);
            if (shutDown_ || sleeping_ || quit_) saveState();
        }
        auto next = progressQueued_.find(id);
        if (next != progressQueued_.end()) {
            ProgressSave queued = std::move(next->second);
            progressQueued_.erase(next);
            if (!shutDown_ && !quit_) sendProgress(id, std::move(queued));
        }
    });
}

void App::checkProgress(TimePoint now) {
    if (!book_ || music_ || !book_->serverBook || book_->link >= servers_.size() ||
        !serverAt(book_->link).syncsProgress() || pendingOpen_) {
        progressHeardSince_.reset();
        return;
    }
    const std::string id = book_->path.string();
    if (id != progressBook_) {
        progressBook_ = id;
        progressHeard_ = {};
        progressLast_ = bookTime(fileIndex_, place());
        progressHeardSince_.reset();
    }
    if (!player_.playing() || player_.buffering()) { progressHeardSince_.reset(); return; }
    if (progressHeardSince_) progressHeard_ += now - *progressHeardSince_;
    progressHeardSince_ = now;
    if (progressHeard_ < kProgressInterval) return;
    const double at = bookTime(fileIndex_, place());
    if (std::abs(at - progressLast_) < kProgressMovement) return;
    pushProgress();
    progressLast_ = at;
    progressHeard_ = {};
}

// Startup retries are silent and never change playback. Durations are fetched
// because the index keeps no per-file timeline for server books.
void App::retryProgress() {
    const std::weak_ptr<int> lifetime = progressLifetime_;
    for (const auto& [key, stamp] : state_.values()) {
        if (!key.starts_with("posat:")) continue;
        const std::string id = key.substr(6);
        const std::string pushed = state_.getString("pushed:" + id);
        try { if (state_.getString("pushdirty:" + id) != "1" && !pushed.empty() && std::stoll(stamp) <= std::stoll(pushed)) continue; }
        catch (...) { continue; }
        const auto on = serverRefOf({id});
        if (!on || !serverAt(on->link).configured() || !serverAt(on->link).syncsProgress()) continue;
        const std::string position = state_.getString("pos:" + id);
        const bool finished = state_.getString("finished:" + id) == "1";
        const size_t link = on->link;
        const std::string prefix = LibraryIndex::seenRef(servers_[link].key, SeenKind::Album, "");
        if (!on->ref.starts_with(prefix)) continue;
        const std::string album = on->ref.substr(prefix.size());
        const auto request = serverAt(link).book(album, [this, lifetime, id, stamp, position, finished, link, album](MusicServer::Error error, RemoteBook book) {
            if (lifetime.expired() || shutDown_ || error != MusicServer::Error::None || book.tracks.empty() ||
                state_.getString("posat:" + id) != stamp || state_.getString("pos:" + id) != position ||
                (state_.getString("finished:" + id) == "1") != finished) return;
            ProgressSave save;
            save.link = link; save.album = album; save.position = position; save.stamp = stamp;
            save.progress.finished = finished;
            try {
                const auto colon = position.find(':');
                if (colon == std::string::npos) return;
                const size_t file = std::stoul(position.substr(0, colon));
                if (file >= book.tracks.size()) return;
                save.progress.seconds = std::stod(position.substr(colon + 1));
                for (size_t i = 0; i < book.tracks.size(); ++i) {
                    save.progress.duration += book.tracks[i].duration;
                    if (i < file) save.progress.seconds += book.tracks[i].duration;
                }
                if (finished) save.progress.seconds = save.progress.duration;
                save.progress.updated = std::stoll(stamp) * kProgressMillisPerSecond;
            } catch (...) { return; }
            sendProgress(id, std::move(save));
        });
        progressRetries_.push_back({link, request});
    }
}

void App::rememberPosition() {
    if (book_ && book_->serverBook) { // its place, like a local book's; no last_stream
        if (!player_.isOpen() && !pendingOpen_ && !reopenAt_) return;
        recordProgress();
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
    return book_->title + ". " + playerStatusBody();
}

// What "Where am I?" says before the status in a player: the player, then what is playing. The music player
// names the album and artist of the current entry (the queue's title names only its first album); the
// book player the book and its author.
std::string App::playerIdentity() const {
    if (!book_) return "";
    std::string album = book_->title, artist = book_->artist;
    if (music_) {
        const QueueEntry* entry = queue_.current();
        album = entry ? entry->album : "";
        artist = entry ? entry->artist : "";
    }
    std::string text = music_ ? "Music player. " : "Book player. ";
    if (!album.empty()) text += album + (artist.empty() ? "" : ", by " + artist) + ". ";
    else if (!artist.empty()) text += "By " + artist + ". ";
    return text;
}

std::string App::playerStatusBody() const {
    if (!book_) return "Nothing playing.";
    auto list = chapters();
    int cur = currentChapter();
    double pos = player_.position();
    std::string text;
    if (list.size() > 1)
        text += std::string(music_ ? "Track " : "Chapter ") + std::to_string(cur + 1) + " of " + std::to_string(list.size()) + ", " +
                list[size_t(cur)].title + ". ";
    if (player_.isOpen()) { // else a remote track opened on Play: no place or length yet
        const bool hasNext = size_t(cur) + 1 < list.size();
        double left = player_.duration() - pos;
        if (hasServerChapters())
            left = hasNext ? bookTime(list[size_t(cur) + 1].file, list[size_t(cur) + 1].start) - bookTime(fileIndex_, pos) : bookLeft();
        else if (trackCount() == 1 && hasNext)
            left = list[size_t(cur) + 1].start - pos;
        text += spokenDuration(left) + " left in " + unit() + ". ";
        if (!music_ && !filePerChapter() && list.size() > 1)
            text += spokenDuration(bookLeft()) + " left in book. ";
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
    cancelStarOptions();
    cancelGestures();
    cancelProgressPull();
    cancelResume();
    cancelLoading();
    cancelSaving();
    cancelPlaylistEdit();
    cancelMarkPing();
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
    cancelGestures();
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
    cancelStarOptions();
    cancelGestures();
    cancelProgressPull();
    cancelResume(); // the front end ticks on while "Goodbye" is spoken
    cancelLoading();
    cancelSaving();
    cancelPlaylistEdit();
    cancelMarkPing(); // Play's answer must not start the music
    dropPendingOpen(); // nothing may start playing while the device shuts down
    pause(false);
    saveOnExit();
    earcons_.play(Earcon::Sleep);
    say("Powering off. Goodbye.");
    quit_ = true;
}

void App::restart(bool saveFirst) {
    if (saveFirst) saveOnExit();
    for (auto [link, request] : progressRetries_) serverAt(link).cancel(request);
    progressRetries_.clear();
    // Ordinary restart keeps the live transport: retain save ordering until
    // its outstanding replies arrive. Factory reset must forget those saves.
    if (!saveFirst) {
        progressLifetime_ = std::make_shared<int>(0);
        progressOut_.clear();
        progressQueued_.clear();
    }
    progressBook_.clear();
    progressHeardSince_.reset();
    progressHeard_ = {};
    shutDown_ = false;
    cancelMarkPing(); // Play's answer must not start the music of the queue that is gone
    pendingOpen_.reset();
    player_.close();
    book_.reset();
    queue_ = Queue{};
    parked_.reset();
    previous_.reset();
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
    config_.clearExcept({kServerPrefix, kServersPrefix});
    state_.clear();
    saveConfig();
    saveState();
    book_.reset(); // the queue is state too
    queue_ = Queue{};
    parked_.reset();
    music_ = false;
    saveQueue();
    say("Settings erased.");
    restart(false); // saving first would put the position straight back
}

std::string App::statusReport() const {
    std::string text;
    if (power_) text += "Battery " + std::to_string(power_->battery().percent) + " percent. ";
    if (storage_) text += std::to_string(storage_->state().freeBytes / (1024ULL * 1024 * 1024)) + " gigabytes free. ";
    text += "Wi-Fi connected. ";
    char buf[32];
    std::time_t t = clock_.wallTime();
    std::strftime(buf, sizeof buf, "Time %H:%M.", std::localtime(&t));
    text += buf;
    for (const auto& error : halErrors_) text += " " + error;
    return text;
}

} // namespace oap
