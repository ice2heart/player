#pragma once

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace oap::test {

// A saved server answer from tests/data/audiobookshelf/.
inline std::string audiobookshelfFixture(const std::string& name) {
    std::ifstream in(std::string(OAP_AUDIOBOOKSHELF_DATA) + "/" + name, std::ios::binary);
    if (!in) ADD_FAILURE() << "missing fixture " << name;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace oap::test
