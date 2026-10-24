#pragma once

#include "core/services.h"

#include <functional>
#include <utility>
#include <vector>

namespace oap {

// The MusicServer of a front end that has no server set up: configured() is
// false, and every request finishes with NotConfigured from the next poll(),
// like a real server with no config. Until the front ends build a Subsonic
// from config.yaml, this is what they give App.
class NoMusicServer : public MusicServer {
public:
    bool configured() const override { return false; }
    RequestId ping(std::function<void(Error, ServerInfo)> done) override;
    RequestId albums(std::string listType, int offset, int size, Done<RemoteAlbum> done) override;
    RequestId artists(Done<RemoteArtist> done) override;
    RequestId artistAlbums(std::string artistId, Done<RemoteAlbum> done) override;
    RequestId albumTracks(std::string albumId, Done<RemoteTrack> done) override;
    RequestId playlists(Done<RemotePlaylist> done) override;
    RequestId playlistTracks(std::string playlistId, Done<RemoteTrack> done) override;
    void cancel(RequestId id) override;
    std::string streamUrl(const std::string&) const override { return {}; }
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
