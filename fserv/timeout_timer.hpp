/* timeout_timer.hpp
   Stores active and inactive client descriptors, notifies of timed-out clients
   by callback */

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace fserv {

    //! @class TimeoutTimer
    /*! Tests entries for exceeding a specified timeout interval, notifies of
     *! timed-out clients via registered callback
     */
    template <typename KeyType>
    class TimeoutTimer {
    public:
        //! Runs the timeout timer
        //! @param timeout_interval
        //!     Timeout interval in milliseconds
        //! @param callback
        //!     Callback function, called on client timeout
        void run(
            int timeout_interval,
            const std::function<void(const std::vector<KeyType*>&)>& callback)
        {
            if (timeout_interval > 0) {
                std::lock_guard<std::mutex> l(status_check_lock_);
                if (worker_.get()) {
                    return;
                }

                is_running_ = true;
                worker_ = std::make_unique<std::thread>(
                    [this, timeout_interval, &callback]() {
                        do_run(timeout_interval, callback);
                    });
            }
        }

        //! Sets or resets given key's timer
        void set(KeyType* key)
        {
            std::lock_guard<std::mutex> l(status_check_lock_);

            const auto now = std::chrono::steady_clock::now();
            keys_[key] = now;
        }

        //! Removes key from timer
        void unset(KeyType* key)
        {
            std::lock_guard<std::mutex> l(status_check_lock_);

            auto itr = keys_.find(key);
            if (itr != keys_.end()) {
                keys_.erase(itr);
            }
        }

        //! Stops the timeout timer
        void stop()
        {
            std::unique_ptr<std::thread> worker;

            {
                std::lock_guard<std::mutex> l(status_check_lock_);
                if (!worker_.get()) {
                    return;
                }

                is_running_ = false;
                worker = std::move(worker_);
            }

            worker->join();
        }
    private:
        //! run() worker
        void do_run(int timeout_interval,
                    std::function<void(const std::vector<KeyType*>&)> callback)
        {
            // 100 us poll interval
            constexpr int kPollInterval = 100;

            // Run loop
            while (true) {
                // Wait for interval
                ::usleep(kPollInterval);

                std::vector<KeyType*> timed_out_keys;

                {
                    std::lock_guard<std::mutex> l(status_check_lock_);
                    if (!is_running_) {
                        break;
                    }

                    timed_out_keys
                        = prune_timed_out_keys(timeout_interval, &keys_);
                }

                if (!timed_out_keys.empty()) {
                    callback(timed_out_keys);
                }
            }
        }

        //! Prunes timed-out keys from passed container
        //! @param timeout_interval
        //!     Timeout interval in milliseconds
        //! @param keys[in/out]
        //!     Container of keys to test for timeout
        //! @return
        //!     All timed-out keys
        static std::vector<KeyType*> prune_timed_out_keys(
            const int timeout_interval,
            std::unordered_map<
                KeyType*,
                std::chrono::time_point<std::chrono::steady_clock>>* keys)
        {
            std::vector<KeyType*> timed_out_keys;

            for (const auto& key: *keys) {
                const std::chrono::time_point<std::chrono::steady_clock>& then
                    = key.second;
                const auto now = std::chrono::steady_clock::now();
                int time_delta
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - then)
                          .count();
                if (time_delta > timeout_interval) {
                    timed_out_keys.push_back(key.first);
                }
            }

            for (const auto& key: timed_out_keys) {
                keys->erase(key);
            }

            return timed_out_keys;
        }

        // All testable keys
        std::unordered_map<KeyType*,
                           std::chrono::time_point<std::chrono::steady_clock>>
            keys_;

        std::mutex status_check_lock_;

        bool is_running_ = false;

        std::unique_ptr<std::thread> worker_;
    };
} // namespace fserv
