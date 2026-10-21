#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace oap::test {

// A temp folder unique to the running test, so `ctest -j` processes never share one.
inline std::filesystem::path testTempDir() {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    return std::filesystem::path(testing::TempDir()) /
           ("oap-" + std::string(info->test_suite_name()) + "." + info->name());
}

} // namespace oap::test
