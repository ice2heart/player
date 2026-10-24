#pragma once

#include <gtest/gtest.h>

#include "core/services.h"

#include <fstream>
#include <ostream>
#include <sstream>
#include <string>

namespace oap {

// So a failed check prints "BadLogin", not the enum's bytes.
inline void PrintTo(MusicServer::Error error, std::ostream* out) {
    static const char* const names[] = {"None", "NotConfigured", "Unreachable", "BadLogin", "Server"};
    *out << names[static_cast<int>(error)];
}

} // namespace oap

namespace oap::test {

// A saved server answer from tests/data/subsonic/.
inline std::string subsonicFixture(const std::string& name) {
    std::ifstream in(std::string(OAP_SUBSONIC_DATA) + "/" + name, std::ios::binary);
    if (!in) ADD_FAILURE() << "missing fixture " << name;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace oap::test
