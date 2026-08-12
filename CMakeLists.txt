cmake_minimum_required(VERSION 3.15)
project(crazyredis CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(crazyredis server.cpp)

find_package(Threads REQUIRED)
target_link_libraries(crazyredis PRIVATE Threads::Threads)

if(WIN32)
  target_link_libraries(crazyredis PRIVATE ws2_32)
endif()

if(MSVC)
  target_compile_options(crazyredis PRIVATE /W4)
else()
  target_compile_options(crazyredis PRIVATE -Wall -Wextra)
endif()
