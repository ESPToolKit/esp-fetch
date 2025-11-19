# ESPFetch - https://github.com/ESPToolKit/esp-fetch
# Copyright zekageri 2025
# MIT License

cmake_minimum_required(VERSION 3.12)
project(ESPFetch)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(${COVERAGE})
    set(CMAKE_CXX_FLAGS "-fprofile-arcs -ftest-coverage -g -O0")
endif()

include_directories(${CMAKE_CURRENT_LIST_DIR}/src)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/test/CMakeLists.txt")
    add_subdirectory(test)
endif()
