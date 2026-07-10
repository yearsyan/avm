// SPDX-License-Identifier: MIT

#ifndef MACMU_SHELL_PENDING_REQUEST_TABLE_H
#define MACMU_SHELL_PENDING_REQUEST_TABLE_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace macmu::shell {

template <typename Key, typename Callback>
class PendingRequestTable {
   public:
    void add(Key key, Callback callback, uint64_t timeout_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[key] = Pending{std::move(callback), steady_now_ms() + timeout_ms};
    }

    Callback take(Key key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(key);
        if (it == pending_.end()) {
            return Callback{};
        }
        Callback callback = std::move(it->second.callback);
        pending_.erase(it);
        return callback;
    }

    template <typename FailureFn>
    void fail_all(FailureFn&& fail) {
        std::map<Key, Pending> failed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failed.swap(pending_);
        }
        for (auto& entry : failed) {
            if (entry.second.callback) {
                fail(std::move(entry.second.callback));
            }
        }
    }

    template <typename FailureFn>
    void sweep_timeouts(FailureFn&& fail) {
        const uint64_t now = steady_now_ms();
        std::vector<Callback> expired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = pending_.begin(); it != pending_.end();) {
                if (it->second.deadlineMs <= now) {
                    expired.push_back(std::move(it->second.callback));
                    it = pending_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& callback : expired) {
            if (callback) {
                fail(std::move(callback));
            }
        }
    }

   private:
    struct Pending {
        Callback callback;
        uint64_t deadlineMs = 0;
    };

    static uint64_t steady_now_ms() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    std::mutex mutex_;
    std::map<Key, Pending> pending_;
};

}  // namespace macmu::shell

#endif  // MACMU_SHELL_PENDING_REQUEST_TABLE_H
