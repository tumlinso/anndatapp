cmake_minimum_required(VERSION 3.28)
project(anndatapp_parallel_streaming_bench LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

if(ANNDATAPP_SOURCE)
  add_subdirectory("${ANNDATAPP_SOURCE}" ann)
else()
  find_package(anndatapp 1.1 CONFIG REQUIRED)
endif()

add_executable(parallel_streaming cpp_driver.cpp)
target_link_libraries(parallel_streaming PRIVATE anndatapp::anndatapp Threads::Threads)
