cmake_minimum_required(VERSION 4.2)
project(Custom_Allocators CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_BUILD_TYPE Release)
add_executable(Custom_Allocators main.cpp)
target_compile_options(Custom_Allocators PRIVATE -O3 -march=native -Wall -Wextra -mavx2)

