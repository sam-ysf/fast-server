#include <atomic>

namespace fserv {

    template <typename Type>
    class ScopedAtomicCounter {
        std::atomic<Type>& ref_;
    public:
        //! @brief Decrements counter
        ~ScopedAtomicCounter();
        //! @brief Increments counter
        explicit ScopedAtomicCounter(std::atomic<Type>& value);
    };

    template <typename Type>
    ScopedAtomicCounter<Type>::~ScopedAtomicCounter()
    {
        ref_.fetch_sub(1);
    }

    template <typename Type>
    ScopedAtomicCounter<Type>::ScopedAtomicCounter(std::atomic<Type>& value)
        : ref_(value)
    {
        ref_.fetch_add(1);
    }
} // namespace fserv