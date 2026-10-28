#pragma once

#include "core/services.h"

#include <filesystem>
#include <memory>
#include <vector>

struct sqlite3;

namespace oap {

// The library index kept in a SQLite file (library.db), behind core's LibraryStore.
//
// Tables: schema_version, items, tracks and refs (the seenOn references of an item
// or of one of its tracks), and playlists (the server's, replaced as a whole by
// savePlaylists()). save() is one transaction. The journal is a plain
// rollback journal (no WAL) and synchronous=FULL, so a power cut leaves the old data.
// The file is created with mode 0600.
//
// A file of the schema version before this one gains the playlists table when it is
// opened. A file that is not a database, is damaged, or has another schema version is moved
// aside (<file>.bad, replacing an earlier one) and the store starts empty: the next
// scan and sync fill it again.
class SqliteLibraryStore : public LibraryStore {
public:
    // Opens or creates `file` (and its folder). If that fails the store is empty and
    // save() returns false.
    explicit SqliteLibraryStore(const std::filesystem::path& file);
    ~SqliteLibraryStore() override;
    SqliteLibraryStore(const SqliteLibraryStore&) = delete;
    SqliteLibraryStore& operator=(const SqliteLibraryStore&) = delete;

    std::vector<IndexItem> load() override;
    bool save(const std::vector<IndexItem>& changed, const std::vector<std::string>& removed) override;
    std::vector<IndexPlaylist> loadPlaylists() override;
    bool savePlaylists(const std::vector<IndexPlaylist>& playlists) override;

    // Where a database that could not be used is moved to.
    static std::filesystem::path asidePath(const std::filesystem::path& file);

private:
    std::filesystem::path file_;
    sqlite3* db_ = nullptr;
};

} // namespace oap
