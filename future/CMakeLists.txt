cmake_minimum_required(VERSION 3.16)
project(future_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(future_tests main.cpp tests.cpp)
target_link_libraries(future_tests PRIVATE Threads::Threads)
