#include "threaded_scanner.h"

#include "core/album_merge.h"
#include "core/library.h"

#include <sys/stat.h>

#include <algorithm>
#include <limits>
#include <exception>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace oap {

namespace {

constexpr char kAlbumSeparator = '\x1f'; // between album artist and album in an album id

// A file's size and the wall time (seconds) it last changed; none if it is gone or is not
// a regular file.
struct FileInfo {
    int64_t size = 0, time = 0;
};
std::optional<FileInfo> statRegular(const fs::path& file) {
    struct stat st {};
    if (::stat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
    return FileInfo{int64_t(st.st_size), int64_t(st.st_mtime)};
}

// One item being built, and whether tags have already given it its title and artist.
struct Group {
    IndexItem item;
    bool fromTags = false;
};

class Scan {
public:
    Scan(TagReader& reader, const ScanRequest& request, std::function<bool()> cancelled)
        : reader_(reader), request_(request), cancelled_(std::move(cancelled)) {
        for (const IndexItem& item : request.previous) {
            if (item.source != IndexItem::Source::Local) continue;
            previous_.emplace(item.id, &item);
            for (const IndexTrack& track : item.tracks) previousTracks_.emplace(track.location, std::make_pair(&item, &track));
        }
    }

    // Every local item; none when cancelled part way.
    std::optional<std::vector<IndexItem>> run() {
        for (const Book& book : scanLibrary(request_.libraryRoot, request_.skip)) {
            if (!addBook(book)) return std::nullopt;
        }
        for (const Book& folder : scanLibrary(request_.musicRoot, request_.skip)) {
            if (!addMusic(folder)) return std::nullopt;
        }
        joinAlbums();
        std::vector<IndexItem> items;
        for (auto& [id, group] : groups_) {
            if (group.item.kind == IndexItem::Kind::Music) sortAlbum(group.item.tracks);
            items.push_back(std::move(group.item));
        }
        return items;
    }

private:
    struct Entry {
        IndexTrack track;
        std::optional<TrackTags> tags; // set when the file was read just now
        std::string keptIn;            // the id of the item a kept track was in
    };

    // The track of `file`: the old entry when its size and file time are the same, else
    // freshly read. None when the file is gone.
    std::optional<Entry> entry(const fs::path& file, IndexItem::Kind kind) {
        auto info = statRegular(file);
        if (!info) return std::nullopt;
        const std::string location = file.string();
        auto old = previousTracks_.find(location);
        if (old != previousTracks_.end() && old->second.first->kind == kind && old->second.second->size == info->size &&
            old->second.second->fileTime == info->time)
            return Entry{*old->second.second, std::nullopt, old->second.first->id};

        Entry e;
        e.tags = reader_.read(file);
        e.track.location = location;
        e.track.size = info->size;
        e.track.fileTime = info->time;
        e.track.title = file.stem().string();
        if (e.tags) {
            if (!e.tags->title.empty()) e.track.title = e.tags->title;
            e.track.artist = e.tags->artist;
            e.track.number = e.tags->track;
            e.track.duration = e.tags->duration;
        }
        return e;
    }

    bool addBook(const Book& book) {
        Group& group = groups_[bookId(book)];
        group.item.id = bookId(book);
        group.item.kind = IndexItem::Kind::Book;
        group.item.title = book.title;
        for (const fs::path& file : book.files) {
            if (cancelled_()) return false;
            if (auto e = entry(file, IndexItem::Kind::Book)) group.item.tracks.push_back(std::move(e->track));
        }
        if (group.item.tracks.empty()) groups_.erase(bookId(book));
        return true;
    }

    bool addMusic(const Book& folder) {
        for (const fs::path& file : folder.files) {
            if (cancelled_()) return false;
            auto e = entry(file, IndexItem::Kind::Music);
            if (!e) continue;
            Group& group = groupFor(*e, folder);
            if (e->tags && !group.fromTags && !e->tags->album.empty()) {
                const TrackTags& t = *e->tags;
                IndexItem& item = group.item;
                item.title = item.album = t.album;
                item.artist = t.albumArtist.empty() ? t.artist : t.albumArtist;
                // One file with fewer tags than the album had must not blank the rest.
                if (!t.genre.empty()) item.genre = t.genre;
                if (t.year != 0) item.year = t.year;
                if (!t.musicBrainzAlbumId.empty()) item.musicBrainzId = t.musicBrainzAlbumId;
                group.fromTags = true;
            }
            group.item.tracks.push_back(std::move(e->track));
        }
        return true;
    }

    // The item a music file belongs to: the one a kept track was in, else the album its
    // tags name, else its folder.
    Group& groupFor(const Entry& e, const Book& folder) {
        std::string id = e.keptIn;
        if (id.empty()) {
            if (e.tags && !e.tags->album.empty()) {
                const std::string& artist = e.tags->albumArtist.empty() ? e.tags->artist : e.tags->albumArtist;
                id = std::string(kLocalAlbumPrefix) + artist + kAlbumSeparator + e.tags->album;
                // No artist names the album: it is the album of its folder (the album folder that
                // scanLibrary() gives, or the folder of a loose file), not of every folder.
                if (artist.empty()) {
                    std::error_code ec;
                    id += kAlbumSeparator + (fs::is_directory(folder.path, ec) ? folder.path : folder.path.parent_path()).string();
                }
            }
            else
                id = std::string(kLocalFolderPrefix) + folder.path.string();
        }
        auto [it, added] = groups_.try_emplace(id);
        Group& group = it->second;
        if (added) {
            auto old = previous_.find(id);
            if (old != previous_.end()) {
                group.item = *old->second; // its tracks are collected again below
                group.item.tracks.clear();
                group.item.firstSeen = group.item.lastPlayed = 0;
            } else {
                group.item.title = folder.title;
            }
            group.item.id = id;
            group.item.source = IndexItem::Source::Local;
            group.item.kind = IndexItem::Kind::Music;
        }
        return group;
    }

    // The files of one album can give different ids: one with no album artist names its track
    // artist ("Band feat. Guest" on one track of "Band"'s album), one has no artist at all, or one
    // spells the artist or the album with other case or spaces. Music items named by an album tag
    // are joined when, with case and spaces folded (artistKey()), the album is the same and
    //  - the artist is the same and not empty (in any folders: CD1 and CD2), or
    //  - they share a folder and one artist is the other followed by more words ("band" and
    //    "band feat. guest"), or
    //  - they share a folder and one has no artist, and the others with that album in that folder
    //    are one album already.
    // Two different artists in one folder ("Singles", the music root) stay apart, and so do albums
    // with no artist in different folders. The rule reads only what the index keeps, so a rescan
    // of kept files gives the same items.
    // The joined item keeps the id from the index with the most tracks (else the first id), so
    // the main album, not a guest's one-track part, keeps its id and play history; it has the
    // artist with the most tracks, the server references of every part, the earliest firstSeen
    // and the latest lastPlayed the index had for them.
    void joinAlbums() {
        std::vector<std::string> ids;
        for (const auto& [id, group] : groups_)
            if (group.item.kind == IndexItem::Kind::Music && !group.item.album.empty()) ids.push_back(id);
        std::vector<size_t> parent(ids.size());
        std::iota(parent.begin(), parent.end(), size_t(0));
        auto root = [&](size_t i) {
            while (parent[i] != i) i = parent[i] = parent[parent[i]];
            return i;
        };
        auto join = [&](size_t i, size_t j) { parent[root(i)] = root(j); };
        auto artistOf = [&](size_t i) { return artistKey(groups_.at(ids[i]).item.artist); };
        auto variant = [](const std::string& a, const std::string& b) { // b is a with more words
            return b.size() > a.size() && b.starts_with(a) && b[a.size()] == ' ';
        };

        std::map<std::string, size_t> byArtist;
        std::map<std::string, std::vector<size_t>> byFolder; // folder + album -> parts
        for (size_t i = 0; i < ids.size(); ++i) {
            const IndexItem& item = groups_.at(ids[i]).item;
            const std::string album = artistKey(item.album);
            const std::string artist = artistOf(i);
            if (!artist.empty()) {
                auto [it, fresh] = byArtist.emplace(artist + kAlbumSeparator + album, i);
                if (!fresh) join(i, it->second);
            }
            std::set<std::string> folders;
            for (const IndexTrack& track : item.tracks)
                folders.insert(fs::path(track.location).parent_path().string() + kAlbumSeparator + album);
            for (const std::string& folder : folders) byFolder[folder].push_back(i);
        }
        for (const auto& [folder, parts] : byFolder)
            for (size_t x = 0; x < parts.size(); ++x)
                for (size_t y = x + 1; y < parts.size(); ++y) {
                    const std::string a = artistOf(parts[x]), b = artistOf(parts[y]);
                    if (!a.empty() && !b.empty() && (variant(a, b) || variant(b, a))) join(parts[x], parts[y]);
                }
        for (const auto& [folder, parts] : byFolder) {
            std::set<size_t> named; // the albums of the parts with an artist
            for (size_t i : parts)
                if (!artistOf(i).empty()) named.insert(root(i));
            if (named.size() != 1) continue;
            for (size_t i : parts)
                if (artistOf(i).empty()) join(i, *named.begin());
        }

        std::map<size_t, std::vector<size_t>> sets;
        for (size_t i = 0; i < ids.size(); ++i) sets[root(i)].push_back(i); // each in id order
        for (const auto& [r, members] : sets) {
            if (members.size() < 2) continue;
            auto tracksOf = [&](size_t i) { return groups_.at(ids[i]).item.tracks.size(); };
            std::optional<size_t> keeper;
            int64_t firstSeen = 0, lastPlayed = 0;
            for (size_t i : members) {
                auto old = previous_.find(ids[i]);
                if (old == previous_.end()) continue;
                if (!keeper || tracksOf(i) > tracksOf(*keeper)) keeper = i;
                const IndexItem& before = *old->second;
                if (before.firstSeen && (!firstSeen || before.firstSeen < firstSeen)) firstSeen = before.firstSeen;
                lastPlayed = std::max(lastPlayed, before.lastPlayed);
            }
            if (!keeper) keeper = members.front();
            IndexItem& item = groups_.at(ids[*keeper]).item;
            std::map<std::string, size_t> tracksBy; // artist key -> tracks
            for (size_t i : members) tracksBy[artistOf(i)] += tracksOf(i);
            std::string artist = item.artist; // the keeper's spelling wins a tie
            for (size_t i : members) {
                const std::string& name = groups_.at(ids[i]).item.artist;
                if (tracksBy[artistKey(name)] > tracksBy[artistKey(artist)]) artist = name;
            }
            for (size_t i : members) {
                if (i == *keeper) continue;
                IndexItem& part = groups_.at(ids[i]).item;
                if (item.genre.empty()) item.genre = part.genre;
                if (item.year == 0) item.year = part.year;
                if (item.musicBrainzId.empty()) item.musicBrainzId = part.musicBrainzId;
                for (const std::string& ref : part.seenOn)
                    if (std::find(item.seenOn.begin(), item.seenOn.end(), ref) == item.seenOn.end()) item.seenOn.push_back(ref);
                for (IndexTrack& track : part.tracks) item.tracks.push_back(std::move(track));
                groups_.erase(ids[i]);
            }
            item.artist = artist;
            item.firstSeen = firstSeen; // 0 unless parts were in the index: the index then keeps the earliest
            item.lastPlayed = lastPlayed;
        }
    }

    // By folder (CD2 before CD10), then by track number, files without one last, then by the
    // order they came in (the file names). The disc number is not stored, so it cannot help.
    static void sortAlbum(std::vector<IndexTrack>& tracks) {
        auto number = [](const IndexTrack& t) { return t.number > 0 ? t.number : std::numeric_limits<int>::max(); };
        std::stable_sort(tracks.begin(), tracks.end(), [&](const IndexTrack& a, const IndexTrack& b) {
            const fs::path pa = fs::path(a.location).parent_path(), pb = fs::path(b.location).parent_path();
            if (pa != pb) return pathLess(pa, pb);
            return number(a) < number(b);
        });
    }

    static std::string bookId(const Book& book) { return std::string(kLocalBookPrefix) + book.path.string(); }

    TagReader& reader_;
    const ScanRequest& request_;
    std::function<bool()> cancelled_;
    std::map<std::string, const IndexItem*> previous_;
    std::map<std::string, std::pair<const IndexItem*, const IndexTrack*>> previousTracks_;
    std::map<std::string, Group> groups_;
};

} // namespace

ThreadedScanner::ThreadedScanner(TagReader& tags) : tags_(tags), worker_([this] { work(); }) {}

ThreadedScanner::~ThreadedScanner() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        runningCancelled_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

LibraryScanner::RequestId ThreadedScanner::scan(ScanRequest request, Done done) {
    const RequestId id = ++lastId_;
    callbacks_.emplace_back(id, std::move(done));
    {
        std::lock_guard lock(mutex_);
        queue_.push_back({id, std::move(request)});
    }
    wake_.notify_one();
    return id;
}

void ThreadedScanner::cancel(RequestId id) {
    std::erase_if(callbacks_, [id](const auto& c) { return c.first == id; });
    std::lock_guard lock(mutex_);
    std::erase_if(queue_, [id](const Job& j) { return j.id == id; });
    std::erase_if(results_, [id](const Result& r) { return r.id == id; });
    if (running_ == id) runningCancelled_ = true;
}

// A callback may start or cancel a scan, so the batch is taken out before it runs.
void ThreadedScanner::poll() {
    std::vector<Result> batch;
    {
        std::lock_guard lock(mutex_);
        batch = std::move(results_);
        results_.clear();
    }
    for (Result& result : batch) {
        auto it = std::find_if(callbacks_.begin(), callbacks_.end(), [&](const auto& c) { return c.first == result.id; });
        if (it == callbacks_.end()) continue; // cancelled by an earlier callback of this batch
        Done done = std::move(it->second);
        callbacks_.erase(it);
        if (done) done(std::move(result.items));
    }
}

bool ThreadedScanner::cancelled(RequestId id) {
    std::lock_guard lock(mutex_);
    return runningCancelled_ && running_ == id;
}

void ThreadedScanner::work() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            running_ = job.id;
            runningCancelled_ = false;
        }
        // A reader that throws (a path that cannot be made absolute, no memory) must not end the
        // program: the scan is dropped and the next one runs. Its callback never comes.
        std::optional<std::vector<IndexItem>> items;
        try {
            items = Scan(tags_, job.request, [this, id = job.id] { return cancelled(id); }).run();
        } catch (const std::exception&) {
        }
        std::lock_guard lock(mutex_);
        if (items && !runningCancelled_) results_.push_back({job.id, std::move(*items)});
        running_ = 0;
    }
}

} // namespace oap
