#include "hal/sim/storage.h"
namespace oap {
std::unique_ptr<Storage> makeSimStorage() { return std::make_unique<SimStorage>(); }
}
