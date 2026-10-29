#include "percent_encode.h"

#include <cctype>

namespace oap {

std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) && c < 0x80) out += char(c);
        else if (c == '-' || c == '.' || c == '_' || c == '~') out += char(c);
        else out += '%', out += hex[c >> 4], out += hex[c & 0xf];
    }
    return out;
}

void trimTrailingSlashes(std::string& url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
}

} // namespace oap
