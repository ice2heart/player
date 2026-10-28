#include "frontend/make_music_server.h"

#include "frontend/no_downloader.h"
#include "frontend/no_music_server.h"
#ifdef OAP_HAVE_NET
#include "net/http_downloader.h"
#include "net/subsonic.h"
#endif

#include <cstdio>

namespace oap {

std::unique_ptr<MusicServer> makeMusicServer(const Settings& config) {
    std::unique_ptr<MusicServer> server;
#ifdef OAP_HAVE_NET
    server = Subsonic::fromConfig(config);
#endif
    if (server) return server;
    if (serverConfigured(config)) {
#ifdef OAP_HAVE_NET
        std::fprintf(stderr, "server: type '%s' is not supported; streaming is off\n",
                     config.getString("server.type").c_str());
#else
        std::fprintf(stderr, "server: this build has no network support (OAP_BUILD_NET=OFF); streaming is off\n");
#endif
    }
    return std::make_unique<NoMusicServer>();
}

void finishMusicServer(MusicServer& server, std::chrono::milliseconds deadline) {
#ifdef OAP_HAVE_NET
    if (auto* subsonic = dynamic_cast<Subsonic*>(&server)) subsonic->finish(deadline);
#else
    (void)server;
    (void)deadline;
#endif
}

std::unique_ptr<Downloader> makeDownloader(const Settings& config) {
#ifdef OAP_HAVE_NET
    if (Subsonic::handles(config))
        return std::make_unique<HttpDownloader>(Subsonic::downloadUrls(Subsonic::configFrom(config)));
#else
    (void)config;
#endif
    return std::make_unique<NoDownloader>();
}

} // namespace oap
