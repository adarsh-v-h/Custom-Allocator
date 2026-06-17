cmake_minimum_required(VERSION 3.22)
project(Custom_Allocators CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)
add_executable(bench bench.cpp)
target_compile_options(bench PRIVATE -O3 -march=native -Wall -Wextra -mavx2)

