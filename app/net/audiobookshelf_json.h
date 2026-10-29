#pragma once

#include "core/services.h"

#include <string>
#include <string_view>
#include <vector>

// Reading Audiobookshelf's JSON answers with yaml-cpp (JSON is YAML 1.2). Pure
// functions on the response body, so they are tested on saved responses. None
// of them reads the user's `token` that /api/me carries.
namespace oap::audiobookshelf {

using Error = MusicServer::Error;

template <class T> struct Parsed {
    Error error = Error::None;
    std::vector<T> items; // empty on an error
    bool more = false;    // false on an error
};

struct Library {
    std::string id, name;
};

struct PingResult {
    Error error = Error::None;
    MusicServer::ServerInfo info;
};

struct BookResult {
    Error error = Error::None;
    RemoteBook book; // empty on an error
};

// GET /status: type "Audiobookshelf" and `serverVersion`. A body of another app is Server.
PingResult parseStatus(std::string_view body);
// GET /api/me: None for a user object, else Server. The answer is only checked, never kept.
Error parseMe(std::string_view body);
// GET /api/libraries: the libraries of media type `book`, by displayOrder (a podcast library is left out).
Parsed<Library> parseLibraries(std::string_view body);
// GET /api/libraries/<id>/items: `results`. `more` is true while offset + size < total.
Parsed<RemoteAlbum> parseBookList(std::string_view body, int offset, int size);
// GET /api/libraries/<id>/authors: `authors`, with the number of books.
Parsed<RemoteArtist> parseAuthors(std::string_view body);
// GET /api/authors/<id>?include=items: the author's `libraryItems`.
Parsed<RemoteAlbum> parseAuthorBooks(std::string_view body);
// GET /api/items/<id>?expanded=1: the files (ids "<item id>/<ino>") and the chapters.
BookResult parseBook(std::string_view body);

struct ProgressResult {
    Error error = Error::None;
    std::optional<RemoteProgress> progress; // empty on an error
};
ProgressResult parseProgress(std::string_view body);
// JSON PATCH body, or none for a non-finite or negative position/duration.
std::optional<std::string> progressBody(const RemoteProgress& progress);

// Milliseconds since 1970 as ISO 8601 UTC ("2026-10-21T08:09:10Z"); empty for 0 or less.
std::string isoTime(long long millis);

} // namespace oap::audiobookshelf
