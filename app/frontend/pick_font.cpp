#include "frontend/pick_font.h"

namespace oap {

std::optional<std::string> pickFont(const std::vector<std::string>& candidates,
                                    const std::function<bool(const std::string&)>& exists) {
    for (const auto& path : candidates)
        if (!path.empty() && exists(path)) return path;
    return std::nullopt;
}

} // namespace oap
