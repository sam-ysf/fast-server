/* timeout_timer.hpp
   Stores active and inactive client descriptors, notifies of timed-out clients
   by callback */

#pragma once

#include "atomic_stack.hpp"
#include "client_generation_token.hpp"
#include <chrono>
#include <cstdint>
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
            std::vector<std::atomic<ClientGenerationToken>>* generation_tokens,
            const std::function<
                void(const std::vector<std::pair<StackNode<KeyType>*,
                                                 std::uint64_t>>&)>& on_timeout)
        {
            if (timeout_interval > 0) {
                std::scoped_lock l(status_check_lock_);
                if (worker_) {
                    return;
                }

                is_running_ = true;
                worker_ = std::make_unique<std::jthread>(
                    [this, timeout_interval, generation_tokens, on_timeout]() {
                        do_run(timeout_interval, generation_tokens, on_timeout);
                    });
            }
        }

        //! Sets given key's timer
        //! @param key
        //!     Key whose timer should be initialized
        //! @param uuid
        //!     Generation identifier for the key
        void set(StackNode<KeyType>* key, std::uint64_t uuid)
        {
            std::scoped_lock l(status_check_lock_);

            const auto now = std::chrono::steady_clock::now();
            keys_[key] = now;
            uuids_[key] = uuid;
        }

        //! Resets given key's timer
        //! @param key
        //!     Key whose timer should be reset
        //! @param uuid
        //!     Generation identifier for the key
        void reset(StackNode<KeyType>* key, std::uint64_t uuid)
        {
            std::scoped_lock l(status_check_lock_);

            if (keys_.contains(key) && uuids_[key] == uuid) {
                const auto now = std::chrono::steady_clock::now();
                keys_[key] = now;
            }
        }

        //! Removes key from timer
        //! @param key
        //!     Key whose timer should be removed
        void unset(StackNode<KeyType>* key, std::uint64_t uuid)
        {
            std::scoped_lock l(status_check_lock_);

            auto itr_uuid = uuids_.find(key);
            if (itr_uuid == uuids_.end()) {
                return;
            }

            if (itr_uuid != uuids_.end()) {
                const auto& [_, v] = *itr_uuid;
                // Check for UUID mismatch
                if (v != uuid) {
                    return;
                }

                uuids_.erase(itr_uuid);
            }

            if (auto itr = keys_.find(key); itr != keys_.end()) {
                keys_.erase(itr);
            }
        }

        //! Stops the timeout timer
        void stop()
        {
            std::unique_ptr<std::jthread> worker;

            {
                std::scoped_lock l(status_check_lock_);
                if (!worker_) {
                    return;
                }

                is_running_ = false;
                worker = std::move(worker_);
            }

            worker->join();
        }
    private:
        //! run() worker
        void do_run(
            int timeout_interval,
            std::vector<std::atomic<ClientGenerationToken>>* generation_tokens,
            std::function<void(const std::vector<
                               std::pair<StackNode<KeyType>*, std::uint64_t>>&)>
                on_timeout)
        {
            // 100 us poll interval
            constexpr int kPollInterval = 100000;

            struct timespec spec = {};
            spec.tv_nsec = kPollInterval;

            // Run loop
            while (true) {
                // Wait for interval
                if (::nanosleep(&spec, nullptr) == -1) {
                    break;
                }

                std::vector<std::pair<StackNode<KeyType>*, std::uint64_t>>
                    timed_out_keys;

                {
                    std::scoped_lock l(status_check_lock_);
                    if (!is_running_) {
                        break;
                    }

                    timed_out_keys = prune_timed_out_keys(
                        timeout_interval, *generation_tokens, keys_, uuids_);
                }

                if (!timed_out_keys.empty()) {
                    on_timeout(timed_out_keys);
                }
            }
        }

        //! Prunes timed-out keys from passed container
        //! @param timeout_interval
        //!     Timeout interval in milliseconds
        //! @param keys[in/out]
        //!     Container of keys to test for timeout
        //! @param keys[in/out]
        //!     Container of key uuids to test for timeout
        //! @return
        //!     All timed-out keys
        static std::vector<std::pair<StackNode<KeyType>*, std::uint64_t>>
        prune_timed_out_keys(
            int timeout_interval,
            std::vector<std::atomic<ClientGenerationToken>>& generation_tokens,
            std::unordered_map<
                StackNode<KeyType>*,
                std::chrono::time_point<std::chrono::steady_clock>>& keys,
            std::unordered_map<StackNode<KeyType>*, std::uint64_t>& uuids)
        {
            std::vector<std::pair<StackNode<KeyType>*, std::uint64_t>>
                timed_out_keys;

            for (const auto& [key, then]: keys) {
                const auto now = std::chrono::steady_clock::now();
                std::int64_t time_delta
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - then)
                          .count();
                if (time_delta > timeout_interval) {
                    using enum ClientState;

                    auto& token = generation_tokens[key->index];

                    ClientGenerationToken expected
                        = {.uuid = key->uuid,
                           .state = static_cast<std::uint64_t>(kReady)};

                    ClientGenerationToken next
                        = {.uuid = key->uuid,
                           .state = static_cast<std::uint64_t>(kStopped)};

                    if (token.compare_exchange_strong(expected, next)) {
                        timed_out_keys.push_back(
                            std::make_pair(key, key->uuid));
                    }
                }
            }

            for (const auto& [key, _]: timed_out_keys) {
                keys.erase(key);
                uuids.erase(key);
            }

            return timed_out_keys;
        }

        // All testable keys
        std::unordered_map<StackNode<KeyType>*,
                           std::chrono::time_point<std::chrono::steady_clock>>
            keys_;

        std::unordered_map<StackNode<KeyType>*, std::uint64_t> uuids_;

        std::mutex status_check_lock_;

        bool is_running_ = false;

        std::unique_ptr<std::jthread> worker_;
    };
} // namespace fserv
