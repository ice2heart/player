#include "text.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace oap {

std::string spokenDuration(double seconds) {
    long total = std::lround(std::max(0.0, seconds));
    long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    auto unit = [](long n, const char* name) {
        return std::to_string(n) + " " + name + (n == 1 ? "" : "s");
    };
    std::vector<std::string> parts;
    if (h) parts.push_back(unit(h, "hour"));
    if (m) parts.push_back(unit(m, "minute"));
    if (!h && (s || parts.empty())) parts.push_back(unit(s, "second")); // seconds only matter under an hour
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : " ") + p;
    return out;
}

} // namespace oap
