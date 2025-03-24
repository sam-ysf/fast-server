/* atomic_stack.hpp -- v1.0
   Thread-safe stack with lock-free concurrency */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>

namespace fserv::util {

    //! @struct StackNode
    /*! Linked-list node, stack component
     */
    template <typename ValueType>
    struct StackNode : ValueType {
        // Unique index
        int uuid = 0;
        // Socket descriptor
        int sfd = 0;
        // Next node in stack
        StackNode<ValueType>* ptr_to_next;
        // Ctor.
        explicit StackNode(StackNode<ValueType>* next_ptr)
            : ptr_to_next(next_ptr)
        {}
    };

    //! @class atomic_stack
    /*! Thread-safe stack with lock-free concurrency control
     */
    template <typename ValueType, typename alloc_t>
    class AtomicStack {
    public:
        /*! @brief Initializes stack
         */
        void init(alloc_t& alloc)
        {
            if (!is_allocated(alloc)) {
                return;
            }

            // Allocate memory
            StackNode<ValueType>* data = alloc.ptr_to_mem_slab;

            // Generate linked-list bottom-most node
            new (&data[0]) StackNode<ValueType>(nullptr);
            // Set first list head
            head_.store(&data[0]);

            for (std::int64_t i = 0; ++i < alloc.capacity;) {
                StackNode<ValueType>* curr_ptr = &data[i];
                StackNode<ValueType>* next_ptr = &data[i - 1];

                auto* ptr = new (curr_ptr) StackNode<ValueType>(next_ptr);

                ptr->uuid = i;

                // Set new list head
                head_.store(ptr);
            }
        }

        /*! @brief Pushes node to top of stack.
         */
        void push(ValueType* value)
        {
            auto* new_head = static_cast<StackNode<ValueType>*>(value);

            new_head->ptr_to_next = head_.load();
            while (!atomic_compare_exchange_weak_explicit(
                &head_,
                &new_head->ptr_to_next,
                new_head,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            }
        }

        /*! @brief Pops node from top of stack.
         */
        StackNode<ValueType>* pop()
        {
            StackNode<ValueType>* head = head_.load();

            while (head
                   && !atomic_compare_exchange_weak_explicit(
                       &head_,
                       &head,
                       head->ptr_to_next,
                       std::memory_order_release,
                       std::memory_order_relaxed)) {
            }

            return head;
        }
    private:
        // Head / top of stack
        std::atomic<StackNode<ValueType>*> head_ = nullptr;
    };
} // namespace fserv::util
