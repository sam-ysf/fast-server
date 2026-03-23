/* std_memory.hpp -- v1.0
   Memory allocation backend */

#pragma once

#include "memory_util.hpp"
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fserv {

    //! @struct StdMemory
    /*! Defines a continuous block of heap memory
     */
    template <typename Type>
    struct StdMemory {
        Type* ptr_to_mem_slab = nullptr;
        std::size_t capacity = 0;
    };

    /*! @brief Checks if memory is allocated.
     */
    template <typename Type>
    inline bool is_allocated(const StdMemory<Type>& std_mem)
    {
        return std_mem.ptr_to_mem_slab != nullptr;
    }

    /*! @brief Initializes passed memory block to given size, padded to page
     *!        boundary.
     */
    template <typename Type>
    inline bool std_init(StdMemory<Type>& std_mem, std::size_t size_hint)
    {
        if (is_allocated(std_mem)) {
            return true;
        }

        size_hint = pad_to_page_boundary(size_hint);
        auto* mem = new Type[size_hint];
        if (!mem) {
            return false;
        }

        std_mem.ptr_to_mem_slab = mem;
        std_mem.capacity = size_hint;

        return true;
    }

    /*! @brief Destroys memory block.
     */
    template <typename Type>
    inline void destroy(StdMemory<Type>& std_mem)
    {
        if (is_allocated(std_mem)) {
            delete[] std_mem.ptr_to_mem_slab;
            std_mem.ptr_to_mem_slab = nullptr;
            std_mem.capacity = 0;
        }
    }
} // namespace fserv
