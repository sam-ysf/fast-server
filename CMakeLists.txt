cmake_minimum_required (VERSION 3.22)

set(Project_name FastServer)
set(Elf_name fserv)
project(${Project_name})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -W")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto=auto")

find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
endif()

if (CMAKE_BUILD_TYPE)
  string(TOLOWER ${CMAKE_BUILD_TYPE} BUILD_TYPE)
else()
  set(BUILD_TYPE "debug")
endif()

if (BUILD_TYPE STREQUAL "debug")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0")
endif (BUILD_TYPE STREQUAL "debug")

if (BUILD_TYPE STREQUAL "release")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
endif (BUILD_TYPE STREQUAL "release")

include_directories(${CMAKE_CURRENT_LIST_DIR}/.)
include_directories(${CMAKE_CURRENT_LIST_DIR}/sample/json/single_include)

# Core srcs
file(GLOB Srcs_top
          sample/*.cpp)

set(Srcs ${Srcs_top})
add_executable(${Elf_name} ${Srcs})

target_link_libraries(${Elf_name} LINK_PUBLIC ncurses)
target_link_libraries(${Elf_name} LINK_PUBLIC tinfo)

