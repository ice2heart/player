#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oap {

struct HalError { std::string message; };
using HalEvent = std::variant<HalError>;

// Producers may post from any thread. The owner drains on the UI thread.
class HalEvents {
public:
    explicit HalEvents(std::function<void()> wake = {}) : wake_(std::move(wake)) {}
    void post(HalEvent event) {
        {
            std::lock_guard lock(mutex_);
            events_.push_back(std::move(event));
        }
        if (wake_) wake_();
    }
    std::vector<HalEvent> drain() {
        std::lock_guard lock(mutex_);
        std::vector<HalEvent> result;
        result.swap(events_);
        return result;
    }
private:
    std::mutex mutex_;
    std::vector<HalEvent> events_;
    std::function<void()> wake_;
};

} // namespace oap
