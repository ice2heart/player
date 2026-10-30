#pragma once

#include "core/services.h"

#include <functional>
#include <utility>
#include <vector>

namespace oap {

// The MusicServer of a front end that has no server set up: configured() is
// false, and every request finishes with NotConfigured from the next poll(),
// like a real server with no config. The front ends give it to App when
// config.yaml sets up no server they can use (or oapnet is not built).
class NoMusicServer : public MusicServer {
public:
    bool configured() const override { return false; }
    RequestId playQueue(std::function<void(Error, std::optional<RemoteQueue>)> done) override;
    RequestId savePlayQueue(RemoteQueue queue, std::function<void(Error)> done) override;
    RequestId ping(std::function<void(Error, ServerInfo)> done) override;
    RequestId catalogueVersion(std::function<void(Error, std::string)> done) override;
    RequestId albums(std::string listType, int offset, int size, Done<RemoteAlbum> done,
                     std::string musicFolderId = {}) override;
    RequestId musicFolders(Done<RemoteFolder> done) override;
    RequestId artists(Done<RemoteArtist> done) override;
    RequestId artistAlbums(std::string artistId, Done<RemoteAlbum> done) override;
    RequestId albumTracks(std::string albumId, Done<RemoteTrack> done) override;
    RequestId book(std::string albumId, std::function<void(Error, RemoteBook)> done) override;
    Catalogue catalogue() const override { return Catalogue::Music; }
    bool syncsProgress() const override { return false; }
    RequestId progress(std::string albumId, std::function<void(Error, std::optional<RemoteProgress>)> done) override;
    void saveProgress(std::string albumId, RemoteProgress progress, std::function<void(Error)> done = {}) override;
    RequestId inProgress(Done<RemoteAlbum> done) override;
    bool supportsStars() const override { return false; }
    RequestId trackDetails(std::string trackId, Done<RemoteTrack> done) override;
    RequestId starredTracks(Done<RemoteTrack> done) override;
    RequestId setStarred(std::string trackId, bool starred, std::function<void(Error)> done) override;
    RequestId playlists(Done<RemotePlaylist> done) override;
    RequestId playlistTracks(std::string playlistId, Done<RemoteTrack> done) override;
    RequestId createPlaylist(std::string name, std::vector<std::string> trackIds, std::function<void(Error, std::string)> done) override;
    RequestId addPlaylistTrack(std::string playlistId, std::string trackId, std::function<void(Error)> done) override;
    RequestId removePlaylistTrack(std::string playlistId, std::size_t index, std::function<void(Error)> done) override;
    RequestId deletePlaylist(std::string playlistId, std::function<void(Error)> done) override;
    void cancel(RequestId id) override;
    Stream stream(const std::string&, int) override { return {}; }
    void scrobble(const std::string&, bool, std::optional<std::int64_t>, std::function<void(Error)>) override {}
    void poll() override;

private:
    template <class T> RequestId fail(Done<T> done) {
        return add([done = std::move(done)] { done(Error::NotConfigured, {}, false); });
    }
    RequestId add(std::function<void()> run);

    RequestId lastId_ = 0;
    std::vector<std::pair<RequestId, std::function<void()>>> ready_;   // answered, for the next poll()
    std::vector<std::pair<RequestId, std::function<void()>>> running_; // the batch poll() is running
};

} // namespace oap
