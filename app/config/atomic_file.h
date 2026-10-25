#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace oap {

// Writes `contents` to FILE.tmp with mode 0600, fsyncs it and renames it over
// `file`, so a power cut leaves the old file or the new one, never half of one.
// A symbolic link stays one: the file it points to is replaced. Creates the folder if needed. Prints the reason to stderr and returns false on failure.
bool writeFileAtomically(const std::filesystem::path& file, const std::string& contents);

// The whole file, or nothing when it does not exist or cannot be read.
std::optional<std::string> readFile(const std::filesystem::path& file);

} // namespace oap
