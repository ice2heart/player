#include "no_music_server.h"

#include <algorithm>

namespace oap {

MusicServer::RequestId NoMusicServer::add(std::function<void()> run) {
    ready_.emplace_back(++lastId_, std::move(run));
    return lastId_;
}

MusicServer::RequestId NoMusicServer::ping(std::function<void(Error, ServerInfo)> done) {
    return add([done = std::move(done)] { done(Error::NotConfigured, {}); });
}
MusicServer::RequestId NoMusicServer::catalogueVersion(std::function<void(Error, std::string)> done) {
    return add([done = std::move(done)] { done(Error::NotConfigured, {}); });
}
MusicServer::RequestId NoMusicServer::albums(std::string, int, int, Done<RemoteAlbum> done, std::string) {
    return fail(std::move(done));
}
MusicServer::RequestId NoMusicServer::musicFolders(Done<RemoteFolder> done) { return fail(std::move(done)); }
MusicServer::RequestId NoMusicServer::artists(Done<RemoteArtist> done) { return fail(std::move(done)); }
MusicServer::RequestId NoMusicServer::artistAlbums(std::string, Done<RemoteAlbum> done) { return fail(std::move(done)); }
MusicServer::RequestId NoMusicServer::albumTracks(std::string, Done<RemoteTrack> done) { return fail(std::move(done)); }
MusicServer::RequestId NoMusicServer::playlists(Done<RemotePlaylist> done) { return fail(std::move(done)); }
MusicServer::RequestId NoMusicServer::playlistTracks(std::string, Done<RemoteTrack> done) { return fail(std::move(done)); }
MusicServer::RequestId NoMusicServer::createPlaylist(std::string, std::vector<std::string>, std::function<void(Error, std::string)> done) {
    return add([done = std::move(done)] { done(Error::NotConfigured, {}); });
}

void NoMusicServer::cancel(RequestId id) {
    std::erase_if(ready_, [id](const auto& r) { return r.first == id; });
    std::erase_if(running_, [id](const auto& r) { return r.first == id; });
}

// A callback may make a new request (answered on the next poll) or cancel one
// that is still to run in this batch, so the batch is a member cancel() can reach.
void NoMusicServer::poll() {
    running_ = std::move(ready_);
    ready_.clear();
    while (!running_.empty()) {
        auto run = std::move(running_.front().second);
        running_.erase(running_.begin());
        run();
    }
}

} // namespace oap
