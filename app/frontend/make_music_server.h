#pragma once

#include "core/services.h"
#include "core/settings.h"

#include <chrono>
#include <memory>
#include <vector>

namespace oap {

// How long a front end gives the servers, after App::shutdown(), to send what is
// queued or running (a scrobble) before the program exits.
constexpr std::chrono::seconds kServerFinishDeadline{3};

// The servers a front end gives App, and the objects behind its links. Build it with makeServers() and keep
// it alive as long as the App.
class Servers {
public:
    Servers() = default;
    Servers(Servers&&) = default;
    Servers& operator=(Servers&&) = default;

    // One link per server this build can talk to, in config order. May be empty.
    const std::vector<ServerLink>& links() const { return links_; }

    // Just before exit: let every Subsonic server finish its requests (Subsonic::finish) for up to
    // `deadline` in all. Blocks.
    void finish(std::chrono::milliseconds deadline = kServerFinishDeadline);

private:
    friend Servers makeServers(const Settings& config);
    std::vector<std::unique_ptr<MusicServer>> servers_;
    std::vector<std::unique_ptr<Downloader>> downloaders_;
    std::vector<ServerLink> links_;
};

// The servers of config.yaml (serverConfigs()): a Subsonic server and an HttpDownloader for each
// Subsonic entry when this build has network support (OAP_BUILD_NET, library oapnet). An entry this build cannot
// use (another type, or no network support) gets no link and a line on stderr that says why; so does
// anything serverConfigs() ignored. A link's name is the entry's `name`, else "Server" when only one entry is set
// up, else "Server 1", "Server 2" by its place among the entries. Its key is TrackCache::serverKey(url, user).
Servers makeServers(const Settings& config);

} // namespace oap
