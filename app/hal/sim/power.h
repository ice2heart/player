#pragma once
#include "core/services.h"
#include <memory>
#include <mutex>

namespace oap {
class SimPower final : public Power {
public:
    Caps caps() const override { return {}; }
    BatteryState battery() const override { std::lock_guard lock(mutex_); return state_; }
    void setBattery(BatteryState state) { std::lock_guard lock(mutex_); state_ = state; }
    void requestSuspend() override {}
    void requestPowerOff() override {}
    void requestReboot() override {}
    void setWakeAlarm(std::optional<std::time_t>) override {}
private:
    mutable std::mutex mutex_;
    BatteryState state_{80, false, false, false};
};
std::unique_ptr<Power> makeSimPower();
}
