#pragma once

#include "core/services.h"
#include "core/settings.h"

#include <chrono>
#include <memory>

namespace oap {

// The MusicServer a front end gives App for the server in config.yaml: a
// Subsonic one when this build has network support (OAP_BUILD_NET, library
// oapnet), else, or for another server.type, a NoMusicServer, so the Streaming
// menu says how to set one up. A server that is set up but cannot be used is
// reported on stderr. Never returns nullptr.
std::unique_ptr<MusicServer> makeMusicServer(const Settings& config);

// How long a front end gives the server, after App::shutdown(), to send what is
// queued or running (a scrobble) before the program exits.
constexpr std::chrono::seconds kServerFinishDeadline{3};

// Just before exit: let a Subsonic server finish its requests (Subsonic::finish)
// for up to `deadline`. Any other server has nothing to finish. Blocks.
void finishMusicServer(MusicServer& server, std::chrono::milliseconds deadline = kServerFinishDeadline);

// The Downloader for the same server: an HttpDownloader for a Subsonic server
// when this build has network support, else a NoDownloader. Says nothing on
// stderr (makeMusicServer reports an unusable server). Never returns nullptr.
std::unique_ptr<Downloader> makeDownloader(const Settings& config);

} // namespace oap
