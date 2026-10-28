#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace oap {

// The first of `candidates` for which `exists` is true, or nothing. The caller puts the
// --font path first, so it wins when it exists. `exists` is a parameter so tests need no files.
std::optional<std::string> pickFont(const std::vector<std::string>& candidates,
                                    const std::function<bool(const std::string&)>& exists);

} // namespace oap
