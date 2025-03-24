/* memory_util.hpp -- v1.0
   Memory utilities backend */

#pragma once

#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fserv::util {

    /*! @brief Returns the size padded to the memory page boundary.
     */
    inline std::size_t padd_to_page_boundary(std::size_t size_hint)
    {
        std::size_t pagesize = getpagesize();
        if (size_hint % pagesize)
            size_hint = size_hint + pagesize - (size_hint % pagesize);
        return size_hint;
    }
} // namespace fserv::util
