#pragma once

#include "core/services.h"

#include <string>
#include <string_view>
#include <vector>

// Reading Subsonic's f=json answers with yaml-cpp (JSON is YAML 1.2). Pure
// functions on the response body, so they are tested on saved responses.
namespace oap::subsonic {

using Error = MusicServer::Error;

template <class T> struct Parsed {
    Error error = Error::None;
    std::vector<T> items; // empty on an error
    bool more = false;    // false on an error
};

struct PingResult {
    Error error = Error::None;
    MusicServer::ServerInfo info;
};

// Subsonic error codes this client tells apart. 40 is a wrong user name or
// password; 50 (not allowed) and 70 (not found), like all others, are Server.
constexpr int kWrongCredentials = 40;

// yaml-cpp before 0.9 rejects \uD800-\uDFFF, so emoji written as a surrogate
// pair ("🎵") would fail the whole answer. This rewrites each pair as
// the YAML escape \UXXXXXXXX and a lone surrogate as U+FFFD; nothing else changes.
std::string jsonForYaml(std::string_view json);

PingResult parsePing(std::string_view body);
// getAlbumList2. `more` is true when the page is full (pageSize albums came back).
Parsed<RemoteAlbum> parseAlbumList(std::string_view body, int pageSize);
Parsed<RemoteArtist> parseArtists(std::string_view body);   // getArtists, all indexes in order
Parsed<RemoteAlbum> parseArtist(std::string_view body);     // getArtist: the artist's albums
Parsed<RemoteTrack> parseAlbum(std::string_view body);      // getAlbum: its songs
Parsed<RemotePlaylist> parsePlaylists(std::string_view body);
Parsed<RemoteTrack> parsePlaylist(std::string_view body);   // getPlaylist: its entries

} // namespace oap::subsonic
