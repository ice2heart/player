#include "sqlite_library_store.h"

#include <sqlite3.h>

#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace oap {

namespace {

constexpr int kSchemaVersion = 2; // 2: items.track_count and items.created (4.8); version 1 is moved aside and rebuilt by scan and sync
constexpr mode_t kFileMode = 0600; // the index lists what the user listens to
constexpr int kItemRef = -1;       // `track` of a refs row that belongs to the item itself

constexpr const char* kSchema =
    "CREATE TABLE schema_version (version INTEGER NOT NULL);"
    "CREATE TABLE items ("
    " id TEXT PRIMARY KEY, source INTEGER NOT NULL, kind INTEGER NOT NULL,"
    " title TEXT NOT NULL, artist TEXT NOT NULL, album TEXT NOT NULL, genre TEXT NOT NULL,"
    " year INTEGER NOT NULL, musicbrainz_id TEXT NOT NULL,"
    " first_seen INTEGER NOT NULL, last_played INTEGER NOT NULL,"
    " track_count INTEGER NOT NULL, created INTEGER NOT NULL);"
    "CREATE TABLE tracks ("
    " item_id TEXT NOT NULL, position INTEGER NOT NULL,"
    " location TEXT NOT NULL, title TEXT NOT NULL, artist TEXT NOT NULL,"
    " number INTEGER NOT NULL, duration REAL NOT NULL, size INTEGER NOT NULL, file_time INTEGER NOT NULL,"
    " PRIMARY KEY (item_id, position));"
    "CREATE TABLE refs ("
    " item_id TEXT NOT NULL, track INTEGER NOT NULL, seq INTEGER NOT NULL, ref TEXT NOT NULL,"
    " PRIMARY KEY (item_id, track, seq));";

// A prepared statement, finalized on every path out.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) stmt_ = nullptr;
    }
    ~Stmt() { sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    explicit operator bool() const { return stmt_ != nullptr; }

    void text(int i, const std::string& v) { sqlite3_bind_text64(stmt_, i, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8); }
    void integer(int i, int64_t v) { sqlite3_bind_int64(stmt_, i, v); }
    void real(int i, double v) { sqlite3_bind_double(stmt_, i, v); }
    // Runs a statement that returns no rows.
    bool run() {
        bool ok = stmt_ && sqlite3_step(stmt_) == SQLITE_DONE;
        if (stmt_) sqlite3_reset(stmt_);
        return ok;
    }
    // Next row; false at the end or on an error (see failed()).
    bool row() {
        int rc = sqlite3_step(stmt_);
        failed_ = rc != SQLITE_ROW && rc != SQLITE_DONE;
        return rc == SQLITE_ROW;
    }
    bool failed() const { return failed_; }

    std::string str(int col) const {
        const unsigned char* p = sqlite3_column_text(stmt_, col);
        return p ? std::string(reinterpret_cast<const char*>(p), size_t(sqlite3_column_bytes(stmt_, col))) : std::string();
    }
    int64_t i64(int col) const { return sqlite3_column_int64(stmt_, col); }
    double dbl(int col) const { return sqlite3_column_double(stmt_, col); }

private:
    sqlite3_stmt* stmt_ = nullptr;
    bool failed_ = false;
};

bool exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

// First column of the first row of `sql` as text. Returns the SQLite result code: SQLITE_ROW
// with `out` set, or whatever stopped it.
int scalar(sqlite3* db, const char* sql, std::string& out) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            const unsigned char* p = sqlite3_column_text(stmt, 0);
            out = p ? reinterpret_cast<const char*>(p) : "";
        }
    }
    sqlite3_finalize(stmt);
    return rc;
}

// The file with mode 0600, created empty if there is none. SQLite would create it with
// the umask and leave an existing one as it is.
void secureFile(const std::filesystem::path& file) {
    int fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, kFileMode);
    if (fd < 0) return;
    ::fchmod(fd, kFileMode);
    ::close(fd);
}

enum class Check { Fresh, Ok, Unusable, Error };

// Fresh: no tables, so the schema is to be created. Ok: our schema, our version.
// Unusable: damaged, not a database, or another version: to be moved aside.
// Error: the file could not be read now (locked, no permission, I/O error); it may be fine, so it stays.
Check inspect(sqlite3* db) {
    auto damaged = [](int rc) { return rc == SQLITE_NOTADB || rc == SQLITE_CORRUPT; };
    std::string result;
    int rc = scalar(db, "PRAGMA quick_check", result);
    if (rc != SQLITE_ROW) return damaged(rc) ? Check::Unusable : Check::Error;
    if (result != "ok") return Check::Unusable;
    rc = scalar(db, "SELECT count(*) FROM sqlite_master", result);
    if (rc != SQLITE_ROW) return damaged(rc) ? Check::Unusable : Check::Error;
    if (result == "0") return Check::Fresh;
    rc = scalar(db, "SELECT version FROM schema_version", result); // no such table: not ours
    if (rc != SQLITE_ROW) return damaged(rc) || rc == SQLITE_ERROR ? Check::Unusable : Check::Error;
    return result == std::to_string(kSchemaVersion) ? Check::Ok : Check::Unusable;
}

bool createSchema(sqlite3* db) {
    if (!exec(db, "BEGIN IMMEDIATE")) return false;
    if (!exec(db, kSchema)) {
        exec(db, "ROLLBACK");
        return false;
    }
    Stmt insert(db, "INSERT INTO schema_version (version) VALUES (?)");
    if (!insert) {
        exec(db, "ROLLBACK");
        return false;
    }
    insert.integer(1, kSchemaVersion);
    if (!insert.run() || !exec(db, "COMMIT")) {
        exec(db, "ROLLBACK");
        return false;
    }
    return true;
}

// The statements of one save(), prepared once and used for every item.
class ItemWriter {
public:
    explicit ItemWriter(sqlite3* db)
        : delRefs_(db, "DELETE FROM refs WHERE item_id = ?"),
          delTracks_(db, "DELETE FROM tracks WHERE item_id = ?"),
          delItem_(db, "DELETE FROM items WHERE id = ?"),
          insItem_(db, "INSERT INTO items VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)"),
          insTrack_(db, "INSERT INTO tracks VALUES (?,?,?,?,?,?,?,?,?)"),
          insRef_(db, "INSERT INTO refs VALUES (?,?,?,?)") {}

    bool ready() const { return delRefs_ && delTracks_ && delItem_ && insItem_ && insTrack_ && insRef_; }

    bool remove(const std::string& id) {
        for (Stmt* del : {&delRefs_, &delTracks_, &delItem_}) {
            del->text(1, id);
            if (!del->run()) return false;
        }
        return true;
    }

    bool write(const IndexItem& item) {
        insItem_.text(1, item.id);
        insItem_.integer(2, int(item.source));
        insItem_.integer(3, int(item.kind));
        insItem_.text(4, item.title);
        insItem_.text(5, item.artist);
        insItem_.text(6, item.album);
        insItem_.text(7, item.genre);
        insItem_.integer(8, item.year);
        insItem_.text(9, item.musicBrainzId);
        insItem_.integer(10, item.firstSeen);
        insItem_.integer(11, item.lastPlayed);
        insItem_.integer(12, item.trackCount);
        insItem_.integer(13, item.created);
        if (!insItem_.run() || !writeRefs(item.id, kItemRef, item.seenOn)) return false;
        int position = 0;
        for (const auto& track : item.tracks) {
            insTrack_.text(1, item.id);
            insTrack_.integer(2, position);
            insTrack_.text(3, track.location);
            insTrack_.text(4, track.title);
            insTrack_.text(5, track.artist);
            insTrack_.integer(6, track.number);
            // SQLite stores a NaN as NULL, which the column refuses.
            insTrack_.real(7, std::isfinite(track.duration) ? track.duration : 0.0);
            insTrack_.integer(8, track.size);
            insTrack_.integer(9, track.fileTime);
            if (!insTrack_.run() || !writeRefs(item.id, position, track.seenOn)) return false;
            ++position;
        }
        return true;
    }

private:
    bool writeRefs(const std::string& id, int track, const std::vector<std::string>& refs) {
        int seq = 0;
        for (const auto& ref : refs) {
            insRef_.text(1, id);
            insRef_.integer(2, track);
            insRef_.integer(3, seq++);
            insRef_.text(4, ref);
            if (!insRef_.run()) return false;
        }
        return true;
    }

    Stmt delRefs_, delTracks_, delItem_, insItem_, insTrack_, insRef_;
};

} // namespace

std::filesystem::path SqliteLibraryStore::asidePath(const std::filesystem::path& file) {
    std::filesystem::path aside = file;
    aside += ".bad";
    return aside;
}

SqliteLibraryStore::SqliteLibraryStore(const std::filesystem::path& file) : file_(file) {
    std::error_code ec;
    if (file_.has_parent_path()) std::filesystem::create_directories(file_.parent_path(), ec);

    for (int attempt = 0; attempt < 2; ++attempt) {
        secureFile(file_);
        sqlite3* db = nullptr;
        Check check = Check::Error;
        if (sqlite3_open_v2(file_.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK) {
            // The first read of a file that is not a database fails here, as damage.
            int rc = sqlite3_exec(db, "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr);
            if (rc == SQLITE_OK) rc = sqlite3_exec(db, "PRAGMA synchronous=FULL", nullptr, nullptr, nullptr);
            check = rc == SQLITE_NOTADB || rc == SQLITE_CORRUPT ? Check::Unusable : rc != SQLITE_OK ? Check::Error : inspect(db);
        }
        if (check == Check::Fresh && !createSchema(db)) check = Check::Error;
        if (check == Check::Ok || check == Check::Fresh) {
            db_ = db;
            return;
        }
        sqlite3_close(db);
        // Only a file that is really bad is moved aside; one that cannot be read now (locked,
        // read-only, a folder) is left alone and the store stays unavailable.
        if (check == Check::Error || attempt == 1) break;
        std::filesystem::rename(file_, asidePath(file_), ec);
        if (ec) {
            std::fprintf(stderr, "cannot move %s aside: %s\n", file_.c_str(), ec.message().c_str());
            break;
        }
        std::fprintf(stderr, "library index %s is not usable, moved to %s\n", file_.c_str(), asidePath(file_).c_str());
    }
}

SqliteLibraryStore::~SqliteLibraryStore() { sqlite3_close(db_); }

std::vector<IndexItem> SqliteLibraryStore::load() {
    std::vector<IndexItem> items;
    if (!db_) return items;
    std::map<std::string, size_t> at; // item id -> index in items

    Stmt itemRows(db_, "SELECT id, source, kind, title, artist, album, genre, year, musicbrainz_id, first_seen, last_played, track_count, created "
                       "FROM items ORDER BY id");
    if (!itemRows) return {};
    while (itemRows.row()) {
        int64_t source = itemRows.i64(1), kind = itemRows.i64(2);
        if (source < 0 || source > int(IndexItem::Source::Cached) || kind < 0 || kind > int(IndexItem::Kind::Podcast)) continue;
        IndexItem item;
        item.id = itemRows.str(0);
        item.source = IndexItem::Source(source);
        item.kind = IndexItem::Kind(kind);
        item.title = itemRows.str(3);
        item.artist = itemRows.str(4);
        item.album = itemRows.str(5);
        item.genre = itemRows.str(6);
        item.year = int(itemRows.i64(7));
        item.musicBrainzId = itemRows.str(8);
        item.firstSeen = itemRows.i64(9);
        item.lastPlayed = itemRows.i64(10);
        item.trackCount = int(itemRows.i64(11));
        item.created = itemRows.i64(12);
        at[item.id] = items.size();
        items.push_back(std::move(item));
    }
    if (itemRows.failed()) return {};

    Stmt trackRows(db_, "SELECT item_id, location, title, artist, number, duration, size, file_time "
                        "FROM tracks ORDER BY item_id, position");
    if (!trackRows) return {};
    while (trackRows.row()) {
        auto it = at.find(trackRows.str(0));
        if (it == at.end()) continue;
        IndexTrack track;
        track.location = trackRows.str(1);
        track.title = trackRows.str(2);
        track.artist = trackRows.str(3);
        track.number = int(trackRows.i64(4));
        track.duration = trackRows.dbl(5);
        track.size = trackRows.i64(6);
        track.fileTime = trackRows.i64(7);
        items[it->second].tracks.push_back(std::move(track));
    }
    if (trackRows.failed()) return {};

    Stmt refRows(db_, "SELECT item_id, track, ref FROM refs ORDER BY item_id, track, seq");
    if (!refRows) return {};
    while (refRows.row()) {
        auto it = at.find(refRows.str(0));
        if (it == at.end()) continue;
        IndexItem& item = items[it->second];
        int64_t track = refRows.i64(1);
        if (track == kItemRef) item.seenOn.push_back(refRows.str(2));
        else if (track >= 0 && size_t(track) < item.tracks.size()) item.tracks[size_t(track)].seenOn.push_back(refRows.str(2));
    }
    if (refRows.failed()) return {};
    return items;
}

bool SqliteLibraryStore::save(const std::vector<IndexItem>& changed, const std::vector<std::string>& removed) {
    if (!db_) return false;
    if (changed.empty() && removed.empty()) return true;
    ItemWriter writer(db_);
    if (!writer.ready() || !exec(db_, "BEGIN IMMEDIATE")) return false;
    bool ok = true;
    for (const auto& id : removed) ok = ok && writer.remove(id);
    for (const auto& item : changed) ok = ok && writer.remove(item.id) && writer.write(item);
    if (ok && exec(db_, "COMMIT")) return true;
    exec(db_, "ROLLBACK");
    return false;
}

} // namespace oap
