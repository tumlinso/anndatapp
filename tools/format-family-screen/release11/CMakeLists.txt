cmake_minimum_required(VERSION 3.28)
project(release11_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(anndatapp 1.0 CONFIG REQUIRED)

add_executable(release11-cpp cpp_probe.cpp)
target_link_libraries(release11-cpp PRIVATE anndatapp::anndatapp ${CMAKE_DL_LIBS})
