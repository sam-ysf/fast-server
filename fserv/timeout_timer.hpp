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
            std::vector<ClientGenerationState>* generation_tokens,
            unsigned timeout_interval,
            const std::function<void(
                std::vector<std::pair<StackNode<KeyType>*, std::uint64_t>>&)>
                handle_timeout)
        {
            if (timeout_interval > 0) {
                std::scoped_lock l(access_lock_);
                if (worker_) {
                    return;
                }

                is_running_ = true;
                worker_ = std::make_unique<std::jthread>([this,
                                                          generation_tokens,
                                                          timeout_interval,
                                                          handle_timeout]() {
                    do_run(generation_tokens, timeout_interval, handle_timeout);
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
            std::scoped_lock l(access_lock_);

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
            std::scoped_lock l(access_lock_);

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
            std::scoped_lock l(access_lock_);

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
                std::scoped_lock l(access_lock_);
                if (!worker_) {
                    return;
                }

                keys_ = std::unordered_map<
                    StackNode<KeyType>*,
                    std::chrono::time_point<std::chrono::steady_clock>>();
                uuids_
                    = std::unordered_map<StackNode<KeyType>*, std::uint64_t>();

                is_running_ = false;
                worker = std::move(worker_);
            }

            worker->join();
        }
    private:
        //! run() worker
        void do_run(
            std::vector<ClientGenerationState>* generation_tokens,
            unsigned timeout_interval,
            const std::function<void(
                std::vector<std::pair<StackNode<KeyType>*, std::uint64_t>>&)>&
                handle_timeout)
        {
            // 100 us poll interval
            constexpr unsigned kPollInterval = 100000;

            struct timespec spec = {};
            spec.tv_nsec = kPollInterval;

            // Run loop
            while (true) {
                // Wait for interval
                if ((::nanosleep(&spec, nullptr) == -1) && (errno != EINTR)) {
                    break;
                }

                std::vector<std::pair<StackNode<KeyType>*, std::uint64_t>>
                    clients_to_prune;

                {
                    std::scoped_lock l(access_lock_);
                    if (!is_running_) {
                        break;
                    }

                    for (const auto& [key, then]: keys_) {
                        if (stop_if_client_timed_out(timeout_interval,
                                                     key,
                                                     then,
                                                     *generation_tokens)) {
                            clients_to_prune.emplace_back(key, uuids_[key]);
                        }
                    }

                    for (auto& [key, _uuid]: clients_to_prune) {
                        keys_.erase(key);
                        uuids_.erase(key);
                    }
                }

                handle_timeout(clients_to_prune);
            }
        }

        bool stop_if_client_timed_out(
            unsigned timeout_interval,
            const StackNode<KeyType>* client,
            const std::chrono::time_point<std::chrono::steady_clock>& then,
            std::vector<ClientGenerationState>& generation_tokens) const
        {
            std::int64_t time_delta
                = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - then)
                      .count();
            if (time_delta < timeout_interval) {
                return false;
            }

            using enum ClientState;

            auto& token = generation_tokens[client->index].token;
            if (std::uint64_t n_writers
                = generation_tokens[client->index].n_writers.load();
                n_writers > 0) {
                return false;
            }

            auto [expected_uuid, _1] = token.load();

            ClientGenerationToken expected
                = {.uuid = expected_uuid,
                   .state = static_cast<std::uint8_t>(kReady)};

            ClientGenerationToken next
                = {.uuid = expected_uuid,
                   .state = static_cast<std::uint8_t>(kStopped)};

            return token.compare_exchange_strong(expected, next);
        }

        // All testable keys
        std::unordered_map<StackNode<KeyType>*,
                           std::chrono::time_point<std::chrono::steady_clock>>
            keys_;

        std::unordered_map<StackNode<KeyType>*, std::uint64_t> uuids_;

        mutable std::mutex access_lock_;

        bool is_running_ = false;

        std::unique_ptr<std::jthread> worker_;
    };
} // namespace fserv
