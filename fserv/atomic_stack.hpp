/* atomic_stack.hpp -- v1.0
   Thread-safe stack with lock-free concurrency */

#pragma once

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
    template <typename ClientSinkType>
    struct StackNode {
        // Client handles read and write
        ClientSinkType sink;
        // Creation-order id
        std::uint64_t index = 0;
        // Next node in stack
        StackNode<ClientSinkType>* next = nullptr;
        // Socket descriptor
        int sfd = 0;

        static_assert(std::is_default_constructible_v<ClientSinkType>);
    };

    //! @class atomic_stack
    /*! Thread-safe stack with lock-free concurrency control
     */
    template <typename ClientSinkType, typename AllocType>
    class AtomicStack {
    public:
        /*! @brief Initializes stack
         */
        void init(AllocType& alloc)
        {
            std::size_t concurrent_connection_count = alloc.capacity;
            if (!is_allocated(alloc) || concurrent_connection_count == 0) {
                return;
            }

            // Allocate memory
            StackNode<ClientSinkType>* const data = alloc.ptr_to_mem_slab;
            data->index = 0;
            data->next = nullptr;

            hazard_ptrs_
                = std::vector<HazardPointer>(concurrent_connection_count);

            // Set first list head
            head_.store(data);

            for (std::size_t i = 0; ++i < alloc.capacity;) {
                StackNode<ClientSinkType>* curr_ptr = &data[i];
                StackNode<ClientSinkType>* next_ptr = &data[i - 1];
                curr_ptr->index = i;
                curr_ptr->next = next_ptr;

                // Set new list head
                head_.store(curr_ptr);
            }
        }

        /*! @brief Pushes node to top of stack.
         */
        void push(StackNode<ClientSinkType>* node)
        {
            // Wait until hazard pointer cleared
            hazard_wait(node);

            node->sink = ClientSinkType();
            node->sfd = 0;

            while (true) {
                StackNode<ClientSinkType>* cur_head = head_.load();
                node->next = cur_head;
                if (head_.compare_exchange_strong(cur_head, node)) {
                    break;
                }
            }
        }

        /*! @brief Pops node from top of stack.
         */
        std::pair<StackNode<ClientSinkType>*, std::uint64_t> pop()
        {
            std::uint64_t next_uuid = monotonic_uuid_counter_.fetch_add(1) + 1;

            HazardPointer* hazard
                = get_hazard_pointer(std::this_thread::get_id());
            if (hazard == nullptr) {
                return {nullptr, 0};
            }

            while (true) {
                StackNode<ClientSinkType>* cur_head = head_.load();

                while (true) {
                    StackNode<ClientSinkType>* temp = cur_head;
                    hazard->value.store(cur_head);
                    cur_head = head_.load();

                    if (temp == cur_head) {
                        break;
                    }
                }

                if (cur_head == nullptr) {
                    hazard->value.store(nullptr);
                    hazard->id.store(std::thread::id());
                    break;
                }

                if (head_.compare_exchange_strong(cur_head, cur_head->next)) {
                    hazard->value.store(nullptr);
                    hazard->id.store(std::thread::id());
                    return std::make_pair(cur_head, next_uuid);
                }
            }

            return {nullptr, 0};
        }
    private:
        struct HazardPointer {
            std::atomic<StackNode<ClientSinkType>*> value = nullptr;
            std::atomic<std::thread::id> id;
        };

        // Waits for hazard pointer to be cleared
        void hazard_wait(const StackNode<ClientSinkType>* node)
        {
            std::size_t i = 0;
            do {
                i = 0;
                for (; i < hazard_ptrs_.size(); ++i) {
                    HazardPointer& hazard = hazard_ptrs_[i];
                    if (hazard.value.load() == node) {
                        break;
                    }
                }
            }
            while (i < hazard_ptrs_.size());
        }

        HazardPointer* get_hazard_pointer(const std::thread::id& thread_id)
        {
            for (HazardPointer& hazard: hazard_ptrs_) {
                std::thread::id dummy_id;
                if (hazard.id.compare_exchange_strong(dummy_id, thread_id)) {
                    return &hazard;
                }
            }

            return nullptr;
        }

        // Head / top of stack
        std::atomic<StackNode<ClientSinkType>*> head_ = nullptr;
        // Monotonically-increasing tag
        std::atomic<std::uint64_t> monotonic_uuid_counter_ = 1;

        std::vector<HazardPointer> hazard_ptrs_;
    };
} // namespace fserv
