/* std_memory.hpp -- v1.0
   Memory allocation backend */

#pragma once

#include "memory_util.hpp"
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fserv::util {

    //! @struct StdMemory
    /*! Defines a continuous block of heap memory
     */
    template <typename v_t>
    struct StdMemory {
        v_t* ptr_to_mem_slab = nullptr;
        int capacity = 0;
    };

    /*! @brief Checks if memory is allocated.
     */
    template <typename v_t>
    inline bool is_allocated(const StdMemory<v_t>& std_mem)
    {
        return std_mem.ptr_to_mem_slab != nullptr;
    }

    /*! @brief Initializes passed memory block to given size, padded to page
     *!        boundary.
     */
    template <typename v_t>
    inline bool init(StdMemory<v_t>& std_mem, int size_hint)
    {
        if (is_allocated(std_mem)) {
            return true;
        }

        size_hint = padd_to_page_boundary(size_hint);
        void* mem = ::malloc(sizeof(v_t) * size_hint);
        if (!mem) {
            return false;
        }

        std_mem.ptr_to_mem_slab = static_cast<v_t*>(mem);
        std_mem.capacity = size_hint;

        return true;
    }

    /*! @brief Destroys memory block.
     */
    template <typename v_t>
    inline void destroy(StdMemory<v_t>& std_mem)
    {
        if (is_allocated(std_mem)) {
            std::free(std_mem.ptr_to_mem_slab);
        }
    }
} // namespace fserv::util
