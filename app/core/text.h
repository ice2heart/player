#pragma once

#include <string>

namespace oap {

// "1 hour 4 minutes", "3 minutes 20 seconds", "45 seconds".
std::string spokenDuration(double seconds);

} // namespace oap
