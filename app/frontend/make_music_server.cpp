#include "frontend/make_music_server.h"

#include "core/track_cache.h"
#ifdef OAP_HAVE_NET
#include "net/audiobookshelf.h"
#include "net/http_downloader.h"
#include "net/subsonic.h"
#endif

#include <cstdio>
#include <string>
#include <thread>

namespace oap {

#ifdef OAP_HAVE_NET
namespace {
constexpr const char* kSubsonicType = "subsonic";           // ServerConfig::type
constexpr const char* kAudiobookshelfType = "audiobookshelf"; // ServerConfig::type
}
#endif

Servers makeServers(const Settings& config) {
    std::vector<std::string> warnings;
    const std::vector<ServerConfig> entries = serverConfigs(config, &warnings);
    for (const std::string& warning : warnings) std::fprintf(stderr, "%s\n", warning.c_str());

    Servers out;
#ifdef OAP_HAVE_NET
    for (size_t i = 0; i < entries.size(); ++i) {
        const ServerConfig& entry = entries[i];
        if (entry.type == kAudiobookshelfType) {
            if (entry.apiKey.empty()) {
                std::fprintf(stderr, "servers: Audiobookshelf needs api_key\n");
                continue;
            }
            auto audiobookshelf = std::make_unique<Audiobookshelf>(Audiobookshelf::configFrom(entry));
            // The downloader asks the server for the URL (it holds the key: never printed). It is destroyed
            // before the server (see the member order of Servers), so the pointer outlives every call.
            Audiobookshelf* source = audiobookshelf.get();
            out.servers_.push_back(std::move(audiobookshelf));
            out.downloaders_.push_back(std::make_unique<HttpDownloader>(
                [source](const std::string& trackId, int) { return source->stream(trackId, 0).url; }));
        } else if (entry.type == kSubsonicType) {
            Subsonic::Config subsonic = Subsonic::configFrom(entry);
            out.servers_.push_back(std::make_unique<Subsonic>(subsonic));
            out.downloaders_.push_back(std::make_unique<HttpDownloader>(Subsonic::downloadUrls(subsonic)));
        } else {
            std::fprintf(stderr, "server: type '%s' is not supported; streaming is off\n", entry.type.c_str());
            continue;
        }
        std::string name = !entry.name.empty() ? entry.name
                           : entries.size() == 1 ? std::string("Server")
                                                 : "Server " + std::to_string(i + 1);
        out.links_.push_back({entry, TrackCache::serverKey(entry.url, entry.user), std::move(name), *out.servers_.back(),
                              *out.downloaders_.back()});
    }
#else
    if (!entries.empty()) std::fprintf(stderr, "server: this build has no network support (OAP_BUILD_NET=OFF); streaming is off\n");
#endif
    return out;
}

void Servers::finish(std::chrono::milliseconds deadline) {
#ifdef OAP_HAVE_NET
    // One thread each: a server that hangs must not use up the time of the others.
    std::vector<std::thread> threads;
    for (auto& server : servers_)
        if (auto* subsonic = dynamic_cast<Subsonic*>(server.get()))
            threads.emplace_back([subsonic, deadline] { subsonic->finish(deadline); });
        else if (auto* books = dynamic_cast<Audiobookshelf*>(server.get()))
            threads.emplace_back([books, deadline] { books->finishTransport(deadline); });
    for (std::thread& thread : threads) thread.join();
    // Progress acknowledgements touch App's shared state map: deliver them on
    // this thread only, after all transport drains and scrobble callbacks ended.
    for (auto& server : servers_)
        if (auto* books = dynamic_cast<Audiobookshelf*>(server.get())) books->finish(std::chrono::milliseconds(0));
#else
    (void)deadline;
#endif
}

} // namespace oap
