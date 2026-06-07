cmake_minimum_required(VERSION 3.10)
project(file_handling C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(file_handling file_handling.c)
target_compile_options(file_handling PRIVATE -Wall -Wextra -Wpedantic)
