#pragma once

#include <string>
#include <string_view>

namespace oap {

// MD5 of `data` as 32 lower-case hex digits. Only for the Subsonic auth token,
// which the protocol defines as md5(password + salt); not for anything secret.
std::string md5Hex(std::string_view data);

} // namespace oap
