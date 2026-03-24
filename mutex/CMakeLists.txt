cmake_minimum_required(VERSION 3.15)
project(mutex_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

find_package(Threads REQUIRED)

add_executable(mutex_tests main.cpp tests.cpp)
target_link_libraries(mutex_tests PRIVATE Threads::Threads)
