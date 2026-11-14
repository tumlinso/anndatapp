cmake_minimum_required(VERSION 3.28)
project(anndatapp_format_family_probe LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(PROBE_MODE "anndata" CACHE STRING "Probe target: anndata, anndatapp, or workload")
set_property(CACHE PROBE_MODE PROPERTY STRINGS anndata anndatapp workload)

find_package(anndatapp 1.0 CONFIG REQUIRED)

if(PROBE_MODE STREQUAL "anndata")
  add_executable(format-family-probe import_anndata.cpp)
elseif(PROBE_MODE STREQUAL "anndatapp")
  add_executable(format-family-probe import_anndatapp.cpp)
elseif(PROBE_MODE STREQUAL "workload")
  if(NOT ANNDATAPP_HAS_HDF5)
    message(FATAL_ERROR "The workload probe requires an HDF5-enabled anndatapp install")
  endif()
  add_executable(format-family-probe workload.cpp)
else()
  message(FATAL_ERROR "Unknown PROBE_MODE='${PROBE_MODE}'")
endif()

target_link_libraries(format-family-probe PRIVATE anndatapp::anndatapp)
