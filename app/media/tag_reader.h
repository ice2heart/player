#pragma once

#include "core/services.h"

namespace oap {

// TagReader on FFmpeg: the container's metadata (ID3, Vorbis comments, MP4 atoms) and
// the audio stream's own, keys compared without case. Holds no state, so read() is
// safe to call from any thread, also from several at once (each call has its own
// AVFormatContext).
class FfmpegTagReader : public TagReader {
public:
    std::optional<TrackTags> read(const std::filesystem::path& file) override;
};

} // namespace oap
