/* atomic_stack.hpp -- v1.0
   Thread-safe stack with lock-free concurrency */

#pragma once

#include "client_state.hpp"
#include <atomic>
#include <cstdint>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace fserv {
    //! @struct StackNode
    /*! Linked-list node, stack component
     */
    template <typename ValueType>
    struct StackNode {
        // Client handles read and write
        ValueType value;
        // Allocation-order id
        std::uint64_t uuid = 0;
        // Creation-order id
        std::uint64_t index = 0;
        // Next node in stack
        StackNode<ValueType>* next = nullptr;
        // Socket descriptor
        std::int32_t sfd = 0;
        // Active/closing lifecycle state for lock-free client ownership
        std::atomic<ClientState> state{ClientState::kClosed};

        static_assert(std::is_default_constructible_v<ValueType>);
    };

    //! @class atomic_stack
    /*! Thread-safe stack with lock-free concurrency control
     */
    template <typename ValueType, typename AllocType>
    class AtomicStack {
    public:
        /*! @brief Initializes stack
         */
        void init(AllocType& alloc, int n_max_workers)
        {
            if (!is_allocated(alloc) || n_max_workers == 0) {
                return;
            }

            // Allocate memory
            StackNode<ValueType>* const data = alloc.ptr_to_mem_slab;
            data->index = 0;
            data->next = nullptr;

            hazard_ptrs_ = std::vector<HazardPointer>(
                static_cast<std::uint64_t>(n_max_workers));

            // Set first list head
            head_.store(data);

            for (std::size_t i = 0; ++i < alloc.capacity;) {
                StackNode<ValueType>* curr_ptr = &data[i];
                StackNode<ValueType>* next_ptr = &data[i - 1];
                curr_ptr->index = i;
                curr_ptr->next = next_ptr;

                // Set new list head
                head_.store(curr_ptr);
            }
        }

        /*! @brief Pushes node to top of stack.
         */
        void push(StackNode<ValueType>* node)
        {
            if (node->state.load() != ClientState::kClosing) {
                return;
            }

            // Wait until hazard pointer cleared
            hazard_wait(node);

            node->value = ValueType();
            node->uuid = 0;
            node->sfd = 0;
            node->state.store(ClientState::kClosed, std::memory_order_acq_rel);

            while (true) {
                StackNode<ValueType>* cur_head = head_.load();
                node->next = cur_head;
                if (head_.compare_exchange_strong(cur_head, node)) {
                    break;
                }
            }
        }

        /*! @brief Pops node from top of stack.
         */
        std::pair<StackNode<ValueType>*, std::uint64_t> pop(int sfd,
                                                            int thread_id)
        {
            std::uint64_t next_uuid = monotonic_uuid_counter_.fetch_add(1) + 1;

            auto& hazard = get_hazard_pointer(thread_id);
            while (true) {
                StackNode<ValueType>* cur_head = head_.load();

                while (true) {
                    StackNode<ValueType>* temp = cur_head;
                    hazard.store(cur_head);
                    cur_head = head_.load();

                    if (temp == cur_head) {
                        break;
                    }
                }

                if (cur_head == nullptr) {
                    hazard.store(nullptr);
                    break;
                }

                if (cur_head
                    && head_.compare_exchange_strong(cur_head,
                                                     cur_head->next)) {
                    hazard.store(nullptr);
                    cur_head->value = ValueType();
                    cur_head->uuid = next_uuid;
                    cur_head->sfd = sfd;
                    cur_head->state.store(ClientState::kReady,
                                          std::memory_order_acq_rel);
                    return std::make_pair(cur_head, next_uuid);
                }
            }

            return {nullptr, 0};
        }
    private:
        struct HazardPointer {
            std::atomic<StackNode<ValueType>*> value = nullptr;
            std::atomic<int> thread_id = 0;
        };

        // Waits for hazard pointer to be cleared
        void hazard_wait(const StackNode<ValueType>* node)
        {
            std::size_t i = 0;
            do {
                i = 0;
                for (; i < hazard_ptrs_.size(); ++i) {
                    const HazardPointer& hazard = hazard_ptrs_[i];
                    if (hazard.value.load() == node) {
                        break;
                    }
                }
            }
            while (i < hazard_ptrs_.size());
        }

        std::atomic<StackNode<ValueType>*>& get_hazard_pointer(int thread_id)
        {
            for (HazardPointer& hazard: hazard_ptrs_) {
                if (hazard.thread_id.load() == thread_id) {
                    return hazard.value;
                }
            }

            int dummy_id = 0;
            for (HazardPointer& hazard: hazard_ptrs_) {
                if (hazard.thread_id.compare_exchange_strong(dummy_id,
                                                             thread_id)) {
                    return hazard.value;
                }
            }

            throw std::bad_alloc();
        }

        // Head / top of stack
        std::atomic<StackNode<ValueType>*> head_ = nullptr;
        // Monotonically-increasing tag
        std::atomic<std::uint64_t> monotonic_uuid_counter_ = 1;

        std::vector<HazardPointer> hazard_ptrs_;
    };
} // namespace fserv
