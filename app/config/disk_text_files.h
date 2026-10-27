#pragma once

#include "atomic_file.h"
#include "core/services.h"

namespace oap {

// The TextFiles of the real file system: reads whole files, writes them with
// writeFileAtomically() (temp file, fsync, rename), so a power cut never leaves
// a half-written manifest.
class DiskTextFiles : public TextFiles {
public:
    std::optional<std::string> read(const std::filesystem::path& file) override { return readFile(file); }
    bool writeAtomically(const std::filesystem::path& file, const std::string& contents) override {
        return writeFileAtomically(file, contents);
    }
};

} // namespace oap
