#include "hal/sim/power.h"
namespace oap {
std::unique_ptr<Power> makeSimPower() { return std::make_unique<SimPower>(); }
}
