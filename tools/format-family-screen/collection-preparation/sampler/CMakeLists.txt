cmake_minimum_required(VERSION 3.28)
project(collection_allocation_cpu_sampler LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
find_package(anndatapp 1.0 CONFIG REQUIRED)
add_executable(collection-allocation-cpu-sampler ../allocation_cpu_sampler.cpp)
target_compile_features(collection-allocation-cpu-sampler PRIVATE cxx_std_20)
target_link_libraries(collection-allocation-cpu-sampler PRIVATE anndatapp::anndatapp)
