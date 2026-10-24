#include "frontend/make_music_server.h"

#include "frontend/no_music_server.h"
#ifdef OAP_HAVE_NET
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

} // namespace oap
