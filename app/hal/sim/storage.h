#pragma once
#include "core/services.h"
#include <memory>
#include <mutex>

namespace oap {
class SimStorage final : public Storage {
public:
    Caps caps() const override { return {}; }
    StorageState state() const override { std::lock_guard lock(mutex_); return state_; }
    void setState(StorageState state) { std::lock_guard lock(mutex_); state_ = state; }
    void setMassStorage(bool) override {}
private:
    mutable std::mutex mutex_;
    StorageState state_{10ULL * 1024 * 1024 * 1024, 32ULL * 1024 * 1024 * 1024, false};
};
std::unique_ptr<Storage> makeSimStorage();
}
