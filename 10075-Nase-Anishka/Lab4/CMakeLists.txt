cmake_minimum_required(VERSION 3.10)
project(rbtree_btree CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(rbtree_btree main.cpp)
target_compile_options(rbtree_btree PRIVATE -Wall -Wextra -Wpedantic)
