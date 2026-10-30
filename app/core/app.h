#pragma once

#include "album_merge.h"
#include "buttons.h"
#include "hal.h"
#include "library.h"
#include "library_index.h"
#include "menu.h"
#include "no_downloader.h"
#include "no_music_server.h"
#include "queue.h"
#include "scrobble_file.h"
#include "services.h"
#include "settings.h"
#include "track_cache.h"

#include <array>
#include <deque>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace oap {

// The queue entries the music player has skipped in a row, said once when it reaches one that plays.
struct SkippedRun {
    size_t count = 0;
    std::optional<size_t> first; // the first one skipped
    std::string firstTitle;
    bool serverDown = false; // some were skipped because their server is marked down
    size_t noServer = 0;     // how many were skipped because their server is not set up
};

// The device's "brain": turns button gestures into navigation, playback and speech.
// Platform independent and free of audio code: everything it talks to is an
// interface from services.h, so tests can drive it with fakes. The simulator and
// the real device both drive it through keyDown/keyUp/setHold/tick, all from one thread.
class App {
public:
    enum class Mode { Menu, Player, Sleeping };

    struct NowPlaying {
        bool loaded = false;
        bool playing = false;
        std::string title;
        std::string chapter;
        int chapterIndex = 0;
        int chapterCount = 0;
        double position = 0;
        double duration = 0;
        std::optional<double> sleepTimerLeft;
    };

    // `config` holds what the user sets (config.yaml), `state` the positions and
    // the book to resume (state.tsv). State is written only on exit and on sleep.
    // `servers` are the servers set up, in config order (the objects in them outlive the App); each
    // one is used. A server gives the stream URLs of its remote tracks and its downloader fetches tracks
    // into its cache (Download album / playlist). With none (an empty list) App behaves as with a server that
    // is not configured. `libraryStore` keeps the library
    // index between runs: the App loads it and Library and Music list it.
    // `scanner` lists the local files in the background: start() asks for a scan, and a finished
    // one replaces the local items of the index, which is saved only if it changed. Both must
    // outlive the App. `cacheRoot` is the folder of
    // downloaded tracks (empty: none, and none without `files` either, which reads and
    // writes their manifests); it is left out of Library and Music
    // when it lies inside them.
    App(Services services, std::filesystem::path libraryRoot, std::filesystem::path musicRoot,
        std::filesystem::path cacheRoot = {}, std::filesystem::path queueFile = {},
        std::filesystem::path scrobbleFile = {});

    // How much room is left on the disk of a folder, in bytes; none if that cannot
    // be told. Downloads check it before they start. The default asks
    // std::filesystem::space(); a test sets its own.
    using FreeSpace = std::function<std::optional<std::uintmax_t>(const std::filesystem::path&)>;
    void setFreeSpace(FreeSpace freeSpace) { freeSpace_ = std::move(freeSpace); }
    // Where Shuffle gets its numbers: a random number in [0, bound). The default is a generator seeded from
    // the Clock; a test sets one that makes the shuffle known.
    void setRandom(QueueRandom random) { random_ = std::move(random); }
    // How many bytes the Cache size setting counts a gigabyte as (a test sets a small one).
    void setCacheGigabyte(std::uintmax_t bytes) { cacheGigabyte_ = bytes; }

    void start();
    void keyDown(Button button);
    void keyUp(Button button);
    void setHold(bool on);
    void tick();
    bool quitRequested() const { return quit_; }
    void shutdown(); // writes the state and any changed setting

    // Plays `tracks` from the server like a local album, from `index` on. It
    // says "Loading." and opens the player once the stream has opened, if the
    // user is still on the screen they chose it from; leaving that screen first
    // drops the open. `title` is what the list is called (album or playlist
    // name). Saves no `pos:` key and leaves `last_book` alone. If the track cannot
    // be opened it says "Server not reachable", tries no other track and keeps
    // what was loaded before (paused). Returns false if that happened at once.
    // With a `source` (the album or playlist on the server) the place is kept
    // the queue is kept for a restart like any other (queue.tsv); `position` is where the track starts.
    // `link`: the server the tracks are on (servers_); its url and user go into the queue entries.
    bool playRemote(const std::string& title, const std::vector<RemoteTrack>& tracks, size_t index,
                    std::optional<RemoteSource> source = std::nullopt, double position = 0,
                    const std::string& artist = {}, size_t link = 0);

    // Plays `entries` (local files and server tracks mixed) as the music queue, from `start`, like
    // playRemote(): `title` is what the queue is called. Replaces the queue. False if it failed at once or `entries` is empty.
    bool playQueue(const std::string& title, std::vector<QueueEntry> entries, size_t start = 0);

    // Read-only state for the simulator's visualisation.
    Mode mode() const;
    const std::vector<Screen>& screens() const { return screens_; }
    NowPlaying nowPlaying() const;
    const std::deque<std::string>& speechLog() const { return speechLog_; }
    const std::array<std::array<int, kGestureCount>, kButtonCount>& buttonUsage() const { return usage_; }
    void resetButtonUsage() { usage_ = {}; }
    bool hold() const { return hold_; }
    bool keyDescriber() const { return keyDescriber_; }

private:
    struct ChapterRef {
        std::string title;
        size_t file = 0;
        double start = 0;
    };

    void onGesture(ButtonEvent ev);
    void cancelGestures(bool preservePower = false);
    void cancelNavigation();
    void onButton(ButtonEvent ev);
    void handleMenu(ButtonEvent ev);
    void handlePlayer(ButtonEvent ev);
    void describeKey(ButtonEvent ev);

    // Speech
    void say(const std::string& text);
    std::string itemSpeech(const Screen& screen) const;
    std::string nowPlayingSpeech() const;
    void whereAmI();

    // Screens and menus
    void openMenu(const std::string& title, std::function<std::vector<MenuItem>()> build, size_t index = 0);
    void pushScreen(Screen screen);     // gives it an id
    void closeScreens(size_t keep);     // all but the first `keep`, cancelling their requests
    Screen* findScreen(std::uint64_t id); // nullptr once it has left the stack
    void activate();
    void moveMenu(int direction, Gesture gesture); // Back / Forward: one, ten, or end
    void back();
    void popScreen(); // back() without a sound or speech
    void goHome();
    void enterPlayer();                                 // the screen of the loaded player
    bool lastPlayerIsMusic() const;                     // the player used last
    std::optional<std::string> playerTitle(bool music) const; // loaded, parked or saved for a restart; none if it has nothing
    void openPlayer(bool music);                        // a Home item: enter, continue or resume that player
    bool resumePlayer(bool music);                      // its saved place after a restart; false if none
    void continueParked();                              // the parked player plays again, the loaded one is parked
    void nothingToPlay();
    std::optional<size_t> playerScreenIndex() const; // its index in screens_, if it is on the stack
    void openPowerMenu();
    void saveConfig();
    void saveOnExit(); // exit and sleep: the only times state.tsv is written
    void saveState();  // only if it changed since it was loaded or last written
    // queue.tsv: the music queue, loaded or parked, as text, written only if it differs from what the file
    // holds (the queue.tsv of a restart that nothing has played from yet is kept as it is). Nothing to keep
    // writes an empty file once, if there was one.
    void saveQueue();
    // start(): the music queue of queue.tsv waits in the music player, closed, at its place; nothing plays.
    void restoreQueue();
    void observeQueueSync(bool forcePlace = false);
    void markQueueSyncChanged();
    void pumpQueueSync();
    void applyRemoteQueue(size_t link, const RemoteQueue& remote);
    std::optional<RemoteQueue> queueForServer(size_t link) const;
    void retitleDownloads(Queue& queue) const;
    void markPlayed(const std::string& itemId); // the album plays from Music: in memory until exit or sleep
    std::vector<MenuItem> homeMenu();
    std::vector<MenuItem> libraryMenu();
    std::vector<MenuItem> musicMenu();
    static std::string artistOf(const MergedAlbum& album);
    std::vector<MenuItem> artistsMenu();
    struct PlaylistEntry;
    std::vector<MenuItem> playlistsMenu();             // Music > Playlists: the index and the downloads
    bool hasPlaylists() const;
    void openPlaylist(size_t link, const std::string& id, const std::string& name);
    std::vector<MenuItem> localPlaylistMenu(const std::string& id);
    void createPlaylist(size_t link, bool local);
    void addCurrentToPlaylist(const PlaylistEntry& playlist);
    void removeFromPlaylist(const PlaylistEntry& playlist, size_t index);
    void selectServerPlaylistRemoval(const PlaylistEntry& playlist);
    std::string stalePlaylistKey(const PlaylistEntry& playlist) const;
    void invalidatePlaylistSnapshot(const PlaylistEntry& playlist);
    void changePlaylistCount(const PlaylistEntry& playlist, int delta);
    MenuItem deletePlaylistItem(const PlaylistEntry& playlist);
    void deletePlaylist(const PlaylistEntry& playlist);
    void cancelPlaylistEdit();
    void markPlaylistScreenDone(std::uint64_t screen, const std::string& message);
    std::vector<MenuItem> addToPlaylistMenu();
    std::optional<std::string> playlistTrackId(const PlaylistEntry& playlist, const QueueEntry& track) const;
    void playlistEditDone(MusicServer::Error error, std::function<void(bool)> success);
    std::vector<MenuItem> genresMenu();
    std::vector<MenuItem> recentAlbums(std::int64_t (*when)(const MergedAlbum&), const std::string& empty);
    std::vector<MenuItem> recentlyAddedMenu();
    std::vector<MenuItem> recentlyPlayedMenu();
    std::vector<MenuItem> albumList(std::vector<MergedAlbum> albums, bool byTitle = true); // else as given
    MenuItem albumEntry(const MergedAlbum& album);
    std::vector<MenuItem> albumMenu(const Book& album, const std::string& playedId = {});
    void openLocalAlbum(const Book& album, const std::string& playedId, size_t at); // its list, on track `at`
    Book bookOf(const IndexItem& item) const;
    void openServerBook(const IndexItem& item, bool followServer = false); // Library, "Now playing": plays it from its saved place (its files if downloaded, else "Loading.")
    bool resumeServerBook();                    // "Now playing" / Play with nothing loaded: false when `last_book` names none
    static bool isServerBookId(const std::string& id);
    const IndexItem* savedServerBook() const;   // `last_book` when it names a server book of a server set up
    static Book serverBookOf(const std::string& ref, const std::string& title, const std::string& author,
                             const std::string& albumId, const std::vector<RemoteTrack>& tracks, size_t link,
                             const std::vector<RemoteChapter>& chapters = {});
    static std::vector<Book::Chapter> serverChaptersOf(const std::vector<double>& durations, std::vector<RemoteChapter> chapters);
    // The server an item is of: the first link, in config order, that the item has a reference on, and that reference.
    struct ServerRef {
        size_t link = 0;
        std::string ref; // `<server key>:album:<id>` or the same for a track
    };
    std::optional<ServerRef> serverRefOf(const std::vector<std::string>& seenOn) const;
    struct PlaylistEntry {
        size_t link = 0;
        std::string id, name;
        size_t tracks = 0;
        bool local = false;
        std::vector<std::filesystem::path> localTracks;
    };
    std::vector<PlaylistEntry> playlistEntries() const; // of every server, by name
    void startScan();
    void syncDownloads();   // the pinned albums of the cache as Cached items (docs/streaming.md 4.8)
    void linkToServer();    // local items with no reference that are a server album by the name rule
    void onScanned(std::vector<IndexItem> items);
    // The server catalogue (docs/streaming.md 4.7): for each server in turn, checks the version, fetches
    // the albums when it changed and replaces that server's items of the index. Silent unless Refresh
    // library asked.
    void dropUnsetServers();   // items, playlists and saved versions of servers that are not set up now; the older single-server keys
    void startSync();
    void syncNext();           // the next server that can be synced, else the sync is over
    void onCatalogueVersion(MusicServer::Error error, const std::string& version);
    void fetchBookFolder();
    void fetchBookIds(const std::string& folderId, int offset);
    void fetchCatalogue(int offset);
    void applyCatalogue();
    void fetchPlaylists();
    void finishSync(MusicServer::Error error);
    void refreshLibrary();
    void finishRefresh();
    std::optional<Book> resumableBook(const std::string& path) const;
    MusicServer& serverAt(size_t link); // servers_[link]; the stand-in when there is no list
    const std::string& linkName(size_t link) const; // what Home calls it
    std::vector<MenuItem> serverMenu(size_t link);    // Home's entry of servers_[link]: the Books menu or the Streaming menu
    std::vector<MenuItem> streamingMenu(size_t link); // the browsing menu of a Music server
    std::vector<MenuItem> booksMenu(size_t link);     // the browsing menu of a Books server (catalogue() == Books)
    void openAuthors(size_t link);
    void openInProgress(size_t link);
    void openBookPage(const RemoteAlbum& book, size_t link); // the book's page: Play book, Download book, chapters
    std::vector<MenuItem> bookItems(const std::vector<RemoteAlbum>& books, size_t link);
    std::vector<MenuItem> bookPage(const RemoteAlbum& book, const RemoteBook& remote, size_t link);
    std::vector<MenuItem> sleepTimerMenu();
    size_t sleepTimerIndex() const;
    std::vector<MenuItem> settingsMenu();
    std::vector<MenuItem> systemMenu();
    std::vector<MenuItem> powerMenu();
    std::vector<MenuItem> helpMenu();
    std::vector<MenuItem> playerMenu();
    MenuItem sleepTimerItem();                          // in both Player menus
    MenuItem goToStartItem(const std::string& label);   // the first file of the loaded list
    std::vector<MenuItem> musicPlayerMenu();
    std::vector<MenuItem> bookPlayerMenu();
    std::vector<MenuItem> chaptersMenu();
    MenuItem queueSetting(const std::string& label, std::vector<std::string> names, std::function<size_t()> current,
                          std::function<bool(size_t)> apply); // false: refused, the list stays
    MenuItem shuffleItem();
    MenuItem repeatItem();
    bool setShuffle(QueueShuffle mode); // false while a track the user chose is opening
    void followQueue();

    // The Queue list (docs/queue.md, "The Queue list"); entries are named by their play order position.
    std::vector<MenuItem> queueMenu();
    std::vector<MenuItem> queueEntryOptions(size_t position);
    std::vector<MenuItem> queueMovePositions(size_t from);
    void queuePlayNext(size_t position);
    void queueMove(size_t from, size_t to);
    void queueRemove(size_t position);
    void finishQueueEdit(size_t screensAbove, size_t index, const std::string& said);
    bool queueLocked();
    void clearQueue(const std::string& said);
    void stopPlayer();
    void unloadQueue(const std::string& said);
    MenuItem choice(const std::string& label, const std::string& key,
                    std::vector<std::pair<int, std::string>> options, int fallback,
                    std::function<void(int)> onChange = {});

    // Options on a menu item (docs/queue.md, "Filling it"): Play now, Play next, Add to queue.
    // A Selection is what they act on: the tracks as queue entries, and the list they came from.
    struct Selection {
        std::string title; // what is spoken: "Added Weather, 12 tracks."
        Book list;         // the list the queue is called after (book_ when it plays)
        std::vector<QueueEntry> entries;
        std::string playedId; // Music: the index item Play now marks as played
    };
    enum class QueueAction { PlayNow, PlayNext, AddToQueue, Download };
    // What Options was pressed on. Local things and tracks the list already holds are Ready; an album,
    // playlist or artist of the server has to be loaded first.
    struct Pick {
        enum class Kind { Ready, Album, Playlist, Artist };
        Kind kind = Kind::Ready;
        std::string title, artist, id, playedId; // remote: what to load
        size_t link = 0;                        // remote: the server it is on (servers_)
        std::string info;                       // an album: what Album info says, empty: no such action
        std::function<Selection()> make;        // Ready
    };
    // The load an action waits for. It belongs to the Options screen: leaving that screen, sleep and power
    // off cancel it, so a late answer changes nothing and says nothing.
    struct Loading {
        std::uint64_t screen = 0; // 0: none
        MusicServer::RequestId request = 0;
        Pick pick;
        QueueAction action = QueueAction::PlayNow;
        std::vector<RemoteAlbum> albums; // an artist: its albums, whose tracks are fetched one by one
        size_t next = 0;
        std::vector<RemoteTrack> tracks;
    };
    void openOptions();                              // the Options button
    std::optional<std::uint64_t> starOptions_; // its screen owns the star read or mutation
    void cancelStarOptions();
    struct StarTarget { size_t link; std::string trackId; };
    std::optional<StarTarget> starTargetOf(const QueueEntry& entry) const;
    void loadPlayerStar();
    void changeStar(std::uint64_t screen, size_t link, std::string trackId, bool starred);
    std::vector<MenuItem> playerOptionsMenu();       // Options in the music player: for the playing track
    void goToAlbum();                                // opens the playing track's album, on that track
    void sayPlayerInfo();                            // Album info / Book info: title, parts, where it is
    MenuItem withOptions(MenuItem item, Pick pick);  // `item` gets the Options of `pick`
    std::vector<MenuItem> optionsMenu(const Pick& pick);
    static Pick localTrackPick(const Book& album, size_t track, const std::string& playedId);
    static Pick localAlbumPick(const Book& album, const std::string& playedId);
    Pick remotePick(Pick::Kind kind, const std::string& id, const std::string& title, const std::string& artist,
                    const std::string& playedId = {}, size_t link = 0);
    Selection remoteSelection(const std::string& title, const std::vector<RemoteTrack>& tracks,
                              std::optional<RemoteSource> source, const std::string& artist, bool ofAlbum,
                              const std::string& albumId = {}, size_t link = 0) const;
    void choose(const Pick& pick, QueueAction action);
    static RemoteSource sourceOf(const Pick& pick); // an album or playlist pick: the list it loads
    bool offersDownload(const Pick& pick) const;     // a server album or playlist that is not whole on the device
    void requestNextAlbum();
    void onPickLoaded(std::uint64_t screen, MusicServer::Error error, std::vector<RemoteTrack> tracks);
    void onArtistAlbums(std::uint64_t screen, MusicServer::Error error, std::vector<RemoteAlbum> albums);
    void failLoading(MusicServer::Error error);
    void cancelLoading();
    // Save as playlist (queue.md, step 2.9): the request that is out, and the Queue list it was asked from.
    struct Saving {
        std::uint64_t screen = 0; // 0: none
        MusicServer::RequestId request = 0;
        std::string name;      // "Queue, 27 October 14:05"
        size_t notSaved = 0;   // entries left out
        size_t songCount = 0;  // entries sent
        size_t link = 0;       // the server it is saved on
        std::string server;    // its key
    };
    void saveAsPlaylist();
    void cancelSaving();
    // The server a remote queue entry is of: the link whose url and user it names. None when that server is
    // not set up now (removed from the config, or no server).
    std::optional<size_t> linkOf(const QueueEntry& entry) const;
    std::optional<size_t> trackLink(size_t i) const; // linkOf() of music entry `i`; the tracks of a server book: the server of the book
    void perform(Selection selection, QueueAction action); // goes back to the list, then does it and says so
    void playNow(Selection selection);
    void addToQueue(Selection selection, bool next);
    // Remote tracks as a list (Book) and as queue entries; `ofAlbum`: all are of the album called `title`.
    static Book remoteBook(const std::string& title, const std::vector<RemoteTrack>& tracks,
                           std::optional<RemoteSource> source, const std::string& artist, size_t link = 0);
    // `albumId`: the server album they are all of (`ofAlbum`); else each track names its own.
    std::vector<QueueEntry> remoteEntries(const std::string& title, const std::vector<RemoteTrack>& tracks, bool ofAlbum,
                                          const std::string& artist, const std::string& albumId = {}, size_t link = 0) const;
    bool queuedFrom(const RemoteSource& source, size_t link) const; // a track of that downloaded list is in the music queue

    // Lists loaded from the server (docs/streaming.md). A list opens at once with
    // "Loading" and is filled when the answer comes. The answer finds its screen
    // by id; leaving the screen cancels the request, so a late answer never runs.
    using PageDone = std::function<void(MusicServer::Error, std::vector<MenuItem>, bool more)>;
    using FetchPage = std::function<MusicServer::RequestId(int offset, PageDone done)>;
    struct RemoteList {
        FetchPage fetch;
        size_t link = 0;                   // the server its requests go to (servers_), to cancel them there
        int pageSize = 0;                  // > 0: offer Load more while the server says there is more
        std::string emptyLabel, emptyHelp; // what an empty list says, and what to do
        size_t loaded = 0;                 // the first items of the screen; a status item may follow
        MusicServer::RequestId request = 0; // the page being loaded, 0 when none
        int offset = 0;                     // of that page, or of the page that failed
        bool more = false;                  // a next page is available through Load more
        bool waiting = false;               // the user waits for it: a "Loading" item is shown
        std::string focusKey;               // the item (MenuItem::key) to open on when it has loaded
        MusicServer::Error error = MusicServer::Error::None; // the last page failed
    };
    template <class T>
    static FetchPage fetchPages(std::function<MusicServer::RequestId(int offset, MusicServer::Done<T>)> request,
                                std::function<std::vector<MenuItem>(const std::vector<T>&)> toItems);
    void openRemoteList(size_t link, const std::string& title, FetchPage fetch, int pageSize, std::string emptyLabel,
                        std::string emptyHelp, std::string focusKey = {});
    void requestPage(std::uint64_t screenId, int offset, bool waiting);
    void onPage(std::uint64_t screenId, MusicServer::Error error, std::vector<MenuItem> items, bool more);
    void showStatus(std::uint64_t screenId); // Loading, the error, or why it is empty, after the loaded items
    void openAlbumList(size_t link, const std::string& title, const std::string& listType, int pageSize, bool paged, bool books = false); // `books`: a Books server's list, its items are books
    // `playedId`: the index item to mark as played when it plays (Music).
    // `focusTrack`: the id of the track to open on, once the list has loaded.
    void openRemoteAlbum(const RemoteAlbum& album, const std::string& playedId = {}, const std::string& focusTrack = {},
                         size_t link = 0);
    std::vector<MenuItem> albumItems(const std::vector<RemoteAlbum>& albums, bool withArtist, size_t link);
    std::vector<MenuItem> trackItems(const std::string& title, const std::vector<RemoteTrack>& tracks,
                                     const std::string& playAll, const RemoteSource& source,
                                     const std::string& artist, std::optional<MenuItem> second = std::nullopt,
                                     const std::string& playedId = {}, size_t link = 0);

    // Downloads (docs/streaming.md, step 3.4). One album or playlist at a time, its
    // tracks one after the other, each into its list folder in the cache; the
    // manifest gets the track (pinned) once it is whole. Nothing here belongs to a
    // screen: leaving the menu, Home or playing something does not stop it.
    struct Download {
        bool background = false;
        int bitrate = 0; // captured Download quality; books remain original
        int playlistVersion = 0; // invalidate only the snapshot this download saw
        bool playlistUnique = true;
        struct Item {
            RemoteTrack track;
            size_t number = 0; // its place in the list, from 1
        };
        size_t link = 0; // the server it is fetched from and written for (servers_): one download runs at a time across all of them
        RemoteSource source;
        std::string title, artist; // of the list, kept in its manifest
        std::vector<std::string> order; // the ids of the list as the server has it, without repeats
        std::vector<std::string> fullOrder; // kept playlist order including repeated entries
        std::vector<Item> todo; // what is left to fetch, in list order
        size_t next = 0;        // the item being fetched
        size_t total = 0;       // tracks in the list (a track listed twice counts once)
        size_t finished = 0;    // of those, whole in the cache
        const char* unit = "track"; // what the progress counts: a book is counted in files
        std::vector<RemoteChapter> chapters; // of a server book, kept in its manifest so it plays offline with them
        Downloader::RequestId request = 0;
    };
    void startDownload(size_t link, const std::string& title, const std::string& artist, const std::vector<RemoteTrack>& tracks,
                       const RemoteSource& source, const std::function<void()>& started = {}, const char* unit = "track",
                       const std::vector<RemoteChapter>& chapters = {}, bool background = false); // `started`: once the job is going
    void sayDownloadBusy(const RemoteSource& source, size_t link);
    bool downloading(const RemoteSource& source, size_t link) const; // that list of that server is the download that runs
    void fetchNextTrack();
    void onTrackDownloaded(Downloader::Result result);
    void deleteDropped(const RemoteSource& source, const std::vector<CachedTrack>& dropped, size_t link); // their files, after the manifest was saved
    void failDownload(const RemoteTrack& track, size_t number, const std::string& reason);
    void finishDownload();
    std::string keptPlaylistKey(size_t link, const std::string& id) const;
    std::string keptRevisionKey(size_t link, const std::string& id) const;
    std::string keptReadyKey(size_t link, const std::string& id) const;
    bool setPlaylistKept(size_t link, const std::string& id, const std::string& title, bool keep);
    void scheduleKeptPlaylist(size_t link, const std::string& id, const std::string& title);
    void pumpKeptPlaylists();
    std::string downloadProgress() const; // "Downloading, 3 of 9 tracks", empty when none runs
    std::vector<MenuItem> downloadedMenu(size_t link);                               // Streaming > Downloaded of servers_[link]
    std::vector<MenuItem> downloadedListMenu(const RemoteSource& source, size_t link); // one downloaded album or playlist of that server
    std::vector<MenuItem> downloadedBookMenu(const RemoteSource& source, size_t link); // one downloaded book of a Books server
    MenuItem deleteDownloadItem(const RemoteSource& source, const std::string& title, size_t link); // "Delete download", behind Cancel
    static std::vector<RemoteTrack> completeTracks(const TrackCache& store, const RemoteSource& source, const CachedList& list);
    bool hasDownloads(size_t link) const; // some list has a complete pinned track (a download of the user's): the error lists offer Downloaded
    void deleteDownload(const RemoteSource& source, const std::string& title, size_t link);
    void checkServer(size_t link); // Server status of servers_[link]

    // The remote list that an older version kept in state.tsv `last_stream.*` (queue.tsv replaces it), to
    // resume after a restart. Only for a server set up now (its url and user, found among the links); none
    // once a local book has been opened since, or when it is not readable.
    struct StreamPlace {
        size_t link = 0; // the server it is of: the link whose url and user were saved
        RemoteSource source;
        std::string title; // of the album or playlist
        size_t track = 0;
        std::string trackId;
        double position = 0;
    };
    std::optional<StreamPlace> savedStream() const;
    // "Now playing" on Home, or Play, with nothing loaded: loads the list from the server
    // ("Loading."), then opens the saved track at its place, as if chosen there.
    // Any new screen (or leaving Home), sleep and power off drop it; an error is said.
    void resumeStream();
    void playResumed(const StreamPlace& place, const std::vector<RemoteTrack>& tracks);
    void cancelResume();

    // Playback. A local file opens at once. A remote track opens in the
    // background (MediaPlayer::opening()): the open is kept in pendingOpen_ and
    // tick() finishes it. `opened` runs once the file is open (and playing, with
    // `autoplay`); `undo` runs if it fails or is dropped, and goes back to what
    // was loaded before.
    // Stopped: nothing from there on can play because the server is marked down; the queue is parked at
    // the first entry that was skipped, closed, and the reason is said (parkAtSkipped()).
    enum class OpenResult { Opened, Pending, Failed, Stopped };
    // Why a queue entry cannot play now (availability()).
    enum class Availability { Yes, Gone, NoServer, ServerDown }; // NoServer: a server track whose server is not set up
    // Who asked for an open: what a remote track says, and what drops it.
    enum class Origin {
        Menu,    // chosen in a menu: says "Loading.", and leaving that menu drops it
        Key,     // Play, or a skip in the player: says "Loading."
        Resume,  // Play opens the current track again (closed after a failure or the end): says "Loading."
        Next,    // the next track at the end of one: quiet
        Restore, // what played before a choice that failed: quiet, even if it fails too
    };
    using Then = std::function<void()>;
    bool openBook(const Book& book, bool autoplay); // false if it cannot be opened
    // The saved place `pos:` + book.path: the file and the seconds, none (0, 0) when unknown or out of range.
    std::pair<size_t, double> savedPlace(const Book& book) const;
    bool playServerBook(const Book& book, size_t file, double position, Then opened, bool explicitPlace = false, bool followServer = false);
    bool playMusic(const Book& album, size_t track, Then opened = {},
                   double position = 0); // false if it failed at once
    // Replaces the music queue with `entries` and plays from `track`. `list` is what they came from
    // (its title, and for a server album or playlist its `source` and track details).
    bool playEntries(const Book& list, std::vector<QueueEntry> entries, size_t track, Then opened = {}, double position = 0);
    // Plays `queue` as it is (its current entry, shuffle and repeat) in place of the loaded one.
    bool playKept(const Book& list, Queue queue, Then opened = {}, double position = 0);
    static std::vector<QueueEntry> entriesOf(const Book& list); // the queue entries of a local or server list
    // `queue` is the music queue when `music`; the book player has none. `continuing`: it is the parked player
    // coming back, so the edited queue parked is not one it replaces.
    OpenResult switchTo(const Book& book, bool music, size_t file, double position, bool autoplay, Then opened = {},
                        std::optional<Queue> queue = std::nullopt, bool continuing = false);

    // The tracks of the loaded player: the play order of the queue (music), or the files of the book.
    // fileIndex_ is the current one in both.
    size_t trackCount() const;
    bool trackRemote(size_t i) const;           // a track of the server (its key is the track id)
    std::string trackKey(size_t i) const;       // the file, or the server's track id
    std::string trackTitle(size_t i) const;
    double trackDuration(size_t i) const;       // from the server's listing, 0 if unknown
    std::optional<size_t> upcomingTrack() const; // the track that follows this one at the end of it
    void setFile(size_t i);                     // fileIndex_ and the queue's current entry
    // The index in the list the queue came from (book_) of the remote track `id`, if it is one of its
    // tracks; `hint` is tried first, since a list can hold a track twice.
    std::optional<size_t> listIndexOf(const std::string& id, size_t hint) const;
    std::string unit() const { return music_ ? "track" : "chapter"; } // what one part is called
    OpenResult loadFile(size_t file, double position, bool autoplay, Origin origin, Then opened = {},
                        Then undo = {}); // within the loaded book; a pending open's undo runs only if this fails
    // `skipped`: entries just before `file` that already count in a run of skips (see continueAfterFailure()).
    OpenResult openFile(size_t file, double position, bool autoplay, Origin origin, Then opened, Then undo,
                        SkippedRun skipped = {});
    // Music queue entries that cannot play are skipped (docs/queue.md, "Playing it"). A server that is
    // marked down is tried again in the background; Menu (the user chose it, from a list the server just
    // gave) tries it anyway.
    Availability availability(size_t i, Origin origin) const;
    void addSkipped(SkippedRun& skipped, size_t file, Availability why);
    bool continuesAfterFailure(Origin origin) const;
    void sayUnavailable(SkippedRun& skipped); // "Storm, source unavailable." / "3 tracks, source unavailable."
    OpenResult parkAtSkipped(const SkippedRun& skipped, size_t startFile, double startPosition);
    void continueAfterFailure(size_t file, const std::string& title); // a track of the queue did not open at the end of the one before
    void markServerDown(size_t link);
    void clearServerDown(size_t link);
    bool serverDown(size_t link) const;
    bool remoteAheadMemo(size_t link); // the same, worked out again only when the queue or the current track changed
    bool remoteAhead(size_t link) const; // some entry that can come up (from the current one on, or any with Repeat all) is of that server
    void checkServerMark(Clock::TimePoint now); // tick(): the background pings of the servers marked down
    void startMarkPing(size_t link, bool forPlay);
    std::optional<size_t> linkToCheck(); // Play: the marked-down server to ping first, if any
    void cancelMarkPing();                      // every link's
    void onMarkPing(size_t link, bool answered, bool forPlay);
    int resumeRewind() const;
    void resumeClosedTrack(); // Play: opens the current track again where it was left
    void failOpen(Origin origin, const std::string& title, size_t file, const Then& undo); // a pending open did not open
    void onOpened(bool autoplay, Origin origin, const Then& opened);
    // A remote track did not open: says so (unless `quiet`), then runs `undo`.
    void onCannotPlay(const std::string& title, bool quiet, const Then& undo);
    void finishPendingOpen(); // tick(): the player is done opening
    void giveUpPendingOpen(); // tick(): it takes too long (Player's own cap did not end it)
    void dropPendingOpen();   // closes the player and runs the open's undo, silently
    void leaveEmptyPlayer();  // after an undo that left nothing loaded: off the player screen
    bool willPlay() const;    // playing, or will once the pending open is done
    void enterPlayerFrom(std::uint64_t screenId); // only if the user is still on that screen
    void togglePlay();
    void play();
    void pause(bool announce);
    void startSeeking(Button button, bool deep);
    void seekStep();
    void repeatSeeking(Clock::TimePoint now);
    void finishSeeking(bool announce);
    void changeVolume(int delta, Gesture gesture);
    std::vector<ChapterRef> chapters() const;   // the server's, else a file each, else the file's own marks
    bool hasServerChapters() const;             // a server book that lists chapters
    bool filePerChapter() const;                // the chapters are the files
    double bookTime(size_t file, double at) const;
    double bookLeft() const;
    double secondsIntoChapter(const ChapterRef& chapter) const;
    bool chapterEndsWithFile() const;
    int currentChapter() const;
    bool gotoChapter(int index, Origin origin, Then opened = {}); // false if its file did not open
    void stepChapter(int direction);
    void onFileFinished();
    void onStreamFailed(); // a remote track broke off: stop, and open it there on Play
    void checkBuffering(Clock::TimePoint now); // tick(): the earcon and "Waiting for network" while a stream is dry
    void checkReadAhead(); // tick(): pre-opens the next remote track near the end of this one
    void startTrackPlay(); // the loaded remote track starts a new play (scrobble_)
    void checkScrobble(Clock::TimePoint now); // tick(): tells the server what plays and what has been heard

    // The automatic cache (docs/streaming.md, step 3.7). A streamed track that has
    // been heard for kAutoCacheAfter is queued here, unpinned, into the folder of the
    // list it plays from. One transfer at a time, only while no explicit download runs
    // (starting one cancels the transfer, which goes back to the head of the queue and
    // resumes its original .part or restarts its transcode later). Silent: no speech,
    // no earcon, failures are ignored.
    struct AutoCache {
        RemoteSource source;
        std::string title, artist; // of the list, for a manifest that does not exist yet
        RemoteTrack track;
        std::int64_t playedAt = 0; // wall time the play began: its lastPlayed
        size_t link = 0;           // the server it is fetched from and cached for (servers_)
        int bitrate = 0;           // the quality used when the stream was opened
    };
    void checkAutoCache();       // tick(): queues the track that has been heard long enough
    void pumpAutoCache();        // tick(): starts the next queued track if nothing else downloads
    void cancelAutoRequest();    // autoRequest_, on the downloader of autoJob_'s server
    void pauseAutoCache();       // an explicit download starts: the running transfer waits
    void dropAutoCache(const RemoteSource& list, size_t link); // a list of that server is deleted: nothing of it is fetched
    void onAutoCached(Downloader::Result result);
    void trimCache(bool flushPlays); // evicts over the limit; flushPlays: the plays in memory to the manifests
    std::vector<std::filesystem::path> partsInUse() const; // .part files of the running and waiting transfers
    std::string trackDestination(const RemoteSource& source, const RemoteTrack& track, size_t link, int bitrate = 0) const; // where a track is downloaded to
    // A fetched track into its list's manifest and the index (shared by both kinds of download).
    struct ManifestUpdate {
        std::string title, artist;  // of the list
        bool replaceName = false;   // the list is called `title` even if its manifest has a name
        bool pinned = false;        // a download the user asked for
        std::int64_t playedAt = 0;  // wall time of the play that caused it, 0: none
        const std::vector<std::string>* order = nullptr; // a download: the list's order on the server
        const std::vector<RemoteChapter>* chapters = nullptr; // a download: the chapters the manifest then holds
        int bitrate = 0; // quality of the completed file
    };
    static bool markTracks(CachedList& cached, const char* mark);
    bool addToManifest(const RemoteSource& source, const RemoteTrack& track, const ManifestUpdate& update, size_t link);
    void replaceCachedPath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
    // Open menu actions can still hold a file name from before a quality replacement.
    std::map<std::string, std::string> replacedCacheFiles_;
    void resolveCachedPaths(std::vector<QueueEntry>& entries) const;
    void forgetCached(const std::string& id, const std::filesystem::path& file, size_t link); // its file is gone: the index finds another copy, if any
    std::filesystem::path failedPart_; // the .part of the explicit download that failed last: OK on Download continues it, so a trim leaves it
    std::uintmax_t cacheLimit() const; // bytes the unpinned tracks may take, 0: no automatic cache
    // The downloaded file of remote track `id` (TrackCache::find()), none if there is none.
    // Looks in an index kept in memory (built at the first call, kept up to date by
    // the downloads), so it is cheap enough for every tick. `verify`: also checks that
    // the file is still on disk, which a track about to open needs.
    std::optional<std::filesystem::path> cachedFile(size_t link, const std::string& id, bool verify = false) const;
    const TrackCache& trackCacheOf(size_t link) const; // of server `link`: <cache>/<its key>/; a link that does not exist has a cache that stores nothing
    bool canDownloadFrom(size_t link) const;
    bool upgradeCached(size_t link, const std::filesystem::path& file, int bitrate) const;
    bool openCachedFallback(size_t file, double position, bool autoplay, Origin origin, const Then& opened);
    int streamingBitrate() const; // Streaming quality: kbit/s, 0 for the original file
    // The stream of remote track `file` at the Streaming quality set now: the
    // pre-opened one if it is that track at that quality, else a new one.
    MusicServer::Stream remoteStream(size_t file);
    MusicServer::Stream newStream(size_t file); // from the server, with the length it lists
    struct ProgressSave {
        size_t link = 0;
        std::string album, position, stamp;
        RemoteProgress progress;
    };
    struct ServerPlace {
        RemoteProgress progress;
        size_t file = 0;
        double position = 0;
        int chapter = 0;
    };
    void pullProgress(bool hadLocal, std::string localStamp, bool followServer = false);
    void cancelProgressPull();
    std::optional<ServerPlace> serverPlace(RemoteProgress progress) const;
    void useServerPlace(ServerPlace place);
    MusicServer::RequestId progressPull_ = 0;
    size_t progressPullLink_ = 0;
    std::uint64_t progressPullScreen_ = 0, progressPullGeneration_ = 0;
    std::optional<ServerPlace> serverPlace_;
    void recordProgress(bool finished = false);
    void pushProgress();
    void sendProgress(const std::string& id, ProgressSave save);
    void retryProgress();
    void checkProgress(Clock::TimePoint now);
    std::map<std::string, ProgressSave> progressOut_, progressQueued_;
    std::vector<std::pair<size_t, MusicServer::RequestId>> progressRetries_;
    std::optional<Clock::TimePoint> progressHeardSince_;
    Clock::TimePoint::duration progressHeard_{};
    std::string progressBook_;
    double progressLast_ = 0;
    void rememberPosition(); // in the state map only, written by saveOnExit()
    double place() const;    // where the loaded track is, open or not
    std::string playerStatus() const;     // the title, then playerStatusBody()
    std::string playerStatusBody() const; // track or chapter, time left, playing or paused, sleep timer
    std::string playerIdentity() const;   // "Music player. Dune, by Weather. " for "Where am I?"

    // Power
    void setSleepTimer(int minutes, bool endOfChapter);
    void sleep(const std::string& announcement);
    void wake();
    void powerOff();
    void restart(bool saveFirst = true);
    void factoryReset();
    std::string statusReport() const;

    using TimePoint = Clock::TimePoint;

    Speech& tts_;
    MediaPlayer& player_;
    SoundEffects& earcons_;
    VolumeControl& volume_;
    NoMusicServer noServer_;   // stands in for the server when none is set up
    NoDownloader noDownloader_;
    std::vector<ServerLink> servers_;
    LibraryIndex index_;
    LibraryScanner& scanner_;
    LocalPlaylists* localPlaylists_ = nullptr;
    struct PlaylistEdit {
        std::uint64_t screen = 0;
        size_t link = 0;
        MusicServer::RequestId request = 0;
    } playlistEdit_;
    std::string lastPlaylist_;
    LibraryScanner::RequestId scanRequest_ = 0; // 0: none running
    struct Catalogue {
        size_t link = 0;     // the server being synced (servers_)
        std::string key;     // its key
        std::string version; // what getArtists said, kept in state when the sync is saved
        std::string folder;  // its audiobook_folder at the start, kept in state with the version
        std::set<std::string> bookIds; // the server album ids in that folder
        bool books = false;  // a Books server: everything it lists is a book
        std::vector<IndexItem> items;
        std::vector<IndexPlaylist> savedPlaylists; // made by Save as playlist while the sync runs
        std::map<std::string, IndexPlaylist> editedPlaylists;
        std::set<std::string> deletedPlaylists;
    };
    MusicServer::RequestId catalogueRequest_ = 0; // 0: no request of the sync is out (between two servers too)
    Catalogue catalogue_;
    bool syncRunning_ = false; // some server is being synced, or is next
    size_t syncLink_ = 0;      // the server being synced, or the next to try
    struct SyncFailure {
        size_t link = 0;
        MusicServer::Error error = MusicServer::Error::None;
    };
    std::vector<SyncFailure> syncFailures_; // of this sync
    size_t syncTried_ = 0;                  // servers this sync asked
    struct Refresh { // Music -> Refresh library, while it runs
        bool active = false;
        std::uint64_t screen = 0; // the Music screen it was pressed on: the result is spoken only there
    };
    Refresh refresh_;
    Settings& config_;
    Settings& state_;
    SettingsMap savedState_; // what state.tsv holds
    std::filesystem::path queueFile_; // queue.tsv; empty: the queue is not kept
    std::string queueText_;           // what queue.tsv holds (read, or last written)
    // The music queue was read from queue.tsv and nothing has played from it yet: "Now playing" on Home
    // plays it, as a stream did before (it is not just entered).
    bool restoredQueue_ = false;
    Clock& clock_;
    HalEvents* halEvents_;
    Power* power_;
    Storage* storage_;
    std::vector<std::string> halErrors_;
    std::optional<BatteryState> lastBattery_;
    bool battery20Announced_ = false, battery10Announced_ = false, battery5Announced_ = false;
    bool batteryCriticalHandled_ = false;
    void batteryChanged(const BatteryState& state);
    std::filesystem::path libraryRoot_;
    std::filesystem::path musicRoot_;
    std::filesystem::path cacheRoot_; // downloaded tracks; never scanned as library or music
    FreeSpace freeSpace_;
    QueueRandom random_;
    GestureDetector gestures_;
    struct Seeking {
        Button button;
        bool deep = false;
        bool edgeHeard = false;
        double target = 0;
        TimePoint next;
    };
    std::optional<Seeking> seeking_;
    bool seekAtEnd_ = false; // a user-seek to EOF stays in this file after release

    std::vector<Screen> screens_;
    std::uint64_t lastScreenId_ = 0;
    std::map<std::uint64_t, RemoteList> remoteLists_; // by Screen::id, while the screen is on the stack
    MusicServer::RequestId ping_ = 0;                 // Server status, while it waits; cancelled when the user moves on
    std::uint64_t pingScreen_ = 0;                    // the screen it was asked from
    size_t pingLink_ = 0;                             // the server it was asked of (servers_)
    std::uint64_t resumeScreen_ = 0;    // the Library screen a server book loads for (resume_), 0: none or not tied to a screen
    MusicServer::RequestId resume_ = 0; // resumeStream(): the list being loaded, 0 when none; a new screen cancels it
    size_t resumeLink_ = 0;             // the server it was asked of (servers_)
    Loading loading_;
    Saving saving_;
    std::optional<Download> download_;
    struct KeptJob { size_t link; std::string id, title; std::vector<RemoteTrack> tracks; bool fetched = false; std::uint64_t screen = 0; int version = 0; std::uint64_t generation = 0; };
    std::vector<KeptJob> keptJobs_;
    std::map<std::string, std::uint64_t> keptGenerations_;
    MusicServer::RequestId keptRequest_ = 0;
    size_t keptRequestLink_ = 0;
    std::string keptRequestPlaylist_;
    std::deque<AutoCache> autoQueue_;
    std::optional<AutoCache> autoJob_; // the transfer in flight, if autoRequest_ != 0
    Downloader::RequestId autoRequest_ = 0;
    std::uintmax_t cacheGigabyte_ = std::uintmax_t(1) << 30;
    // When cached tracks last played (track id -> wall time), kept in memory and written
    // to the manifests by trimCache(): on exit and on sleep.
    // (Per server: LinkState::played.)
    bool trimDirty_ = false; // a transfer was abandoned (.part left): the next trim looks even if cacheBytes_ is low
    TextFiles* textFiles_ = nullptr;
    bool sleeping_ = false;
    bool shutDown_ = false; // shutdown() has run: the state is written and the player closed
    bool volumeChanged_ = false;   // changed with the volume keys, written on exit
    bool volumeEdgeHeard_ = false; // a held volume key already played the edge sound at its limit
    // The loaded player: its book (or the album / playlist when music_), the player it is, and
    // its file. There is one MediaPlayer, so only this one is open.
    // For the music player `book_` is the list the queue came from (title, server source and track
    // details); the tracks that play are queue_'s.
    std::optional<Book> book_;
    bool music_ = false; // the music player, else the book player
    Queue queue_;        // the music player's queue; empty for the book player (the parked one is in parked_)
    std::int64_t queueChanged_ = 0;
    std::set<std::string> queuePending_;
    std::map<std::string, std::int64_t> queueAcknowledged_;
    std::uint64_t queueObservedRevision_ = 0, queueSyncGeneration_ = 0;
    double queueObservedPlace_ = 0;
    bool queueSyncInitialized_ = false;
    size_t queueSyncLink_ = 0;
    MusicServer::RequestId queueSyncRequest_ = 0;
    Clock::TimePoint queueSyncNext_{};
    Clock::TimePoint queuePlaceRecorded_{};
    bool queueSyncRound_ = true;
    struct PendingRemoteQueue { size_t link; RemoteQueue value; std::uint64_t generation; };
    std::optional<PendingRemoteQueue> pendingRemoteQueue_;
    size_t fileIndex_ = 0;
    // The other player's book, paused where it stopped (a remote one opens again on Continue). It
    // is of the other kind than book_ (when one is loaded); none until the other player has been used.
    struct Parked {
        Book book;
        bool music = false;
        size_t file = 0;
        double position = 0;
        Queue queue; // the music queue of a parked music player
    };
    std::optional<Parked> parked_;
    // The queue edited by hand that the last replacement kept, for "Previous queue" (queue.md); OK on it
    // brings it back. Set once the new queue has opened.
    struct PreviousQueue {
        Book list;
        Queue queue;
        double position = 0; // in the current entry, seconds
    };
    std::optional<PreviousQueue> previous_;
    std::optional<PreviousQueue> editedQueue() const; // the music queue a new one would replace, if edited by hand
    void restorePrevious();
    // Set while a remote track is loaded but closed: after a failed open (opening
    // it again at once would wait on the same dead server) or at the end of the
    // list. play() opens it at this position.
    std::optional<double> reopenAt_;
    // A remote track that is opening. Its book and file are already book_ and
    // fileIndex_. A newer open, sleep and leaving `screen` drop it (undo).
    struct PendingOpen {
        std::uint64_t screen = 0; // the menu it was chosen in (Origin::Menu); 0: none
        Origin origin = Origin::Menu;
        std::string title; // of the track, for "Cannot play"
        bool autoplay = false; // Play / Pause while it opens changes this
        double position = 0;   // where it opens
        TimePoint since;       // when it was asked for
        Then opened, undo;
        size_t file = 0; // its track
        bool cachedFallback = false; // an upgrade may fall back if its stream cannot open
    };
    std::optional<PendingOpen> pendingOpen_;
    // The stream that plays first ran dry (MediaPlayer::buffering()) at `since`.
    struct Dropout {
        TimePoint since;
        std::optional<TimePoint> playingSince; // audio has come again since then
        bool earcon = false; // the buffering earcon has played
        bool said = false;   // "Waiting for network" has been said
    };
    std::optional<Dropout> dry_;
    // remoteAhead() worked out for this queue stamp, current track and player; checkServerMark() asks every
    // tick and the queue is long.
    struct AheadMemo {
        std::uint64_t revision = 0;
        size_t file = 0;
        bool music = false, loaded = false, configured = false, known = false, ahead = false;
    };
    // What belongs to one server (one per link; one, for no server, when there are none).
    struct LinkState {
        // A server that failed to open a track or whose stream broke off is marked down: its queue entries are
        // skipped until a ping answers. Pings run in the background while the music player has its entries ahead.
        bool down = false;
        TimePoint pingDue;                    // when the next background ping is sent (while down)
        int pingStep = 0;                     // pings that went unanswered: picks the wait to the next
        MusicServer::RequestId markPing = 0;  // in flight, 0: none
        AheadMemo ahead;
        // When cached tracks last played (track id -> wall time), kept in memory and written to the manifests
        // by trimCache(): on exit and on sleep. Track ids are only unique on their own server.
        std::map<std::string, std::int64_t> played;
        // Bytes of unpinned tracks and .part files at the last trim, plus the tracks cached since: while
        // it is known and within the limit, caching a track does not scan the manifests again.
        std::optional<std::uintmax_t> cacheBytes;
        // The complete files of the server, by track id (TrackCache::index()), and its cache, made at the first call.
        std::optional<std::map<std::string, std::filesystem::path>> cacheIndex;
        std::map<std::filesystem::path, int> cacheQualities;
        std::optional<TrackCache> cache;
    };
    LinkState& linkState(size_t link) const; // `link` past the list: the state of the no-server stand-in
    mutable std::vector<LinkState> links_;
    // The track pre-opened (checkReadAhead()), if any. Only opening that track
    // takes the pre-open, and that moves fileIndex_ on to it. The player takes
    // a pre-open over only for the same URL, so opening that track at the same
    // quality reuses this stream (each new one from the server has a new salt).
    struct ReadAhead {
        size_t link = 0; // the server it was opened on: another server may have a track with the same id
        std::string trackId;
        int bitrate = 0;
        MusicServer::Stream stream;
    };
    std::optional<ReadAhead> readAhead_;
    // The play of the remote track loaded now, for scrobbling. It starts when a
    // track opens or restarts from its start, but goes on when the same track
    // opens again where it was (Origin::Resume, Restore), and ends with the
    // track or when a local book opens. Once it plays the server hears "now
    // playing", and once it has been heard long enough (kScrobbleShare /
    // kScrobbleAtMost) that it was played; each once. Local files are never scrobbled.
    struct TrackPlay {
        size_t link = 0; // the server that plays it: it hears the scrobble
        std::string trackId;
        bool nowPlayingSent = false;
        bool submitted = false;
        bool cacheAsked = false; // the automatic cache has had its turn (checkAutoCache())
        std::int64_t startedAt = 0; // wall time it began
        Clock::TimePoint::duration heard{};  // listened time
        std::optional<TimePoint> heardSince; // the last tick that heard it
    };
    std::optional<TrackPlay> scrobble_;
    bool openedFromStream_ = false; // a local fallback must never schedule an upgrade download
    int openedStreamBitrate_ = 0; // captured on open; changing Settings applies to the next track
    // The scrobble outbox: every submission, with the time it was heard, until its server answers it. Kept
    // in scrobbles.tsv (empty path or no `files`: in memory only), written at once and each time it
    // changes, oldest first, at most kMaxScrobbles. Entries of another server stay and are not sent.
    void submitScrobble(size_t link, const std::string& trackId); // into the outbox, then sent
    void loadScrobbles();                             // start(): reads the file
    void saveScrobbles();                             // writes the file if its text changed
    void sendScrobbles();                             // a server answered: every server is tried again, see sendNextScrobble()
    void sendNextScrobble();                          // the oldest entry of a server that is not marked down and did not fail this round, if none is out
    void onScrobbleAnswer(const ScrobbleEntry& sent, MusicServer::Error error);
    std::filesystem::path scrobbleFile_;
    std::string scrobbleText_; // what scrobbles.tsv holds (read, or last written)
    std::vector<ScrobbleEntry> outbox_;
    bool scrobbleOut_ = false; // a submission is sent and not answered
    std::set<size_t> scrobbleFailed_; // servers that could not be reached by a submission since a server last answered: skipped this round

    bool hold_ = false;
    std::array<bool, kButtonCount> held_{};   // raw keys down now
    std::array<bool, kButtonCount> locked_{}; // down when Hold went on: ignored until released
    bool keyDescriber_ = false;
    bool quit_ = false;
    std::optional<TimePoint> pausedAt_;
    std::optional<TimePoint> sleepDeadline_;
    int sleepMinutes_ = 0; // the time chosen for sleepDeadline_
    int sleepAtChapterEnd_ = -1;
    TimePoint lastActivity_;

    std::deque<std::string> speechLog_;
    std::array<std::array<int, kGestureCount>, kButtonCount> usage_{};
    // Save requests outlive App; callbacks first check this token on the UI thread.
    std::shared_ptr<int> progressLifetime_ = std::make_shared<int>(0);
};

} // namespace oap
