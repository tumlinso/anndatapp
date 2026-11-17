cmake_minimum_required(VERSION 3.28)
project(anndatapp VERSION 1.1.0 LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
  message(FATAL_ERROR "The initial supported toolchain is Clang 18 or newer. Configure with -DCMAKE_CXX_COMPILER=clang++-18 and Ninja.")
endif()
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(FATAL_ERROR "Use Ninja (>=1.11) for C++20 module dependency scanning.")
endif()
string(REGEX MATCH "^[0-9]+" _anndatapp_clang_major "${CMAKE_CXX_COMPILER_VERSION}")
if(NOT CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS)
  find_program(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS
    NAMES clang-scan-deps-${_anndatapp_clang_major} clang-scan-deps
    REQUIRED)
endif()
execute_process(COMMAND "${CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS}" --version
  RESULT_VARIABLE _scanner_result OUTPUT_VARIABLE _scanner_version ERROR_VARIABLE _scanner_error)
string(REGEX MATCH "[Vv]ersion[ ]+([0-9]+)" _scanner_match "${_scanner_version}")
if(NOT _scanner_result EQUAL 0 OR NOT _scanner_match OR NOT CMAKE_MATCH_1 STREQUAL _anndatapp_clang_major)
  message(FATAL_ERROR "clang-scan-deps must match Clang ${_anndatapp_clang_major}: ${_scanner_version}${_scanner_error}")
endif()

option(ANNDATAPP_BUILD_TESTS "Build standalone native tests" ${PROJECT_IS_TOP_LEVEL})
option(ANNDATAPP_BUILD_EXAMPLES "Build import-only examples" ${PROJECT_IS_TOP_LEVEL})
option(ANNDATAPP_BUILD_BENCHMARKS "Build numerical memory benchmarks" OFF)
option(ANNDATAPP_ENABLE_INTEROP_TESTS "Compare semantics using Python AnnData fixtures" OFF)
option(ANNDATAPP_ENABLE_FORMAT_INTEROP_TESTS "Check the format family with independent pinned Python references" OFF)
option(ANNDATAPP_ENABLE_RECORD_INTEROP_TESTS "Compare records using the isolated Scanpy reference" OFF)
option(ANNDATAPP_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ANNDATAPP_ENABLE_HDF5 "Enable native H5AD reading and writing" ON)
option(ANNDATAPP_ENABLE_ZARR "Enable native Zarr v2/v3 reading and writing" ON)
option(ANNDATAPP_ENABLE_PARQUET "Enable private SpatialData Parquet/GeoParquet I/O" OFF)
if(ANNDATAPP_ENABLE_RECORD_INTEROP_TESTS AND (NOT ANNDATAPP_ENABLE_INTEROP_TESTS OR NOT ANNDATAPP_ENABLE_HDF5))
  message(FATAL_ERROR "Record interoperability tests require ANNDATAPP_ENABLE_INTEROP_TESTS and ANNDATAPP_ENABLE_HDF5.")
endif()
if(ANNDATAPP_ENABLE_FORMAT_INTEROP_TESTS AND NOT ANNDATAPP_BUILD_TESTS)
  message(FATAL_ERROR "Format interoperability checks require ANNDATAPP_BUILD_TESTS.")
endif()
set(ANNDATAPP_DEPENDENCY_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/build/dependencies/usr" CACHE PATH
  "Optional private prefix containing storage development dependencies")
if(EXISTS "${ANNDATAPP_DEPENDENCY_PREFIX}")
  list(PREPEND CMAKE_PREFIX_PATH "${ANNDATAPP_DEPENDENCY_PREFIX}")
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(ANNDATAPP_ARROW_ROOT "" CACHE PATH "Optional private Arrow/Parquet installation or pyarrow wheel prefix")
if(ANNDATAPP_ENABLE_PARQUET)
  find_package(ArrowParquet REQUIRED)
endif()

if(ANNDATAPP_ENABLE_HDF5 OR ANNDATAPP_ENABLE_ZARR)
  find_package(PkgConfig REQUIRED)
endif()
if(ANNDATAPP_ENABLE_HDF5)
  pkg_check_modules(ANNDATAPP_HDF5 REQUIRED IMPORTED_TARGET hdf5>=1.10)
endif()
if(ANNDATAPP_ENABLE_ZARR)
  pkg_check_modules(ANNDATAPP_ZLIB REQUIRED IMPORTED_TARGET zlib)
  pkg_check_modules(ANNDATAPP_ZSTD REQUIRED IMPORTED_TARGET libzstd)
  find_package(Blosc REQUIRED)
endif()
if(ANNDATAPP_ENABLE_HDF5 OR ANNDATAPP_ENABLE_ZARR)
  find_package(nlohmann_json 3.11 CONFIG REQUIRED)
endif()

add_library(anndatapp_core)
add_library(anndatapp::core ALIAS anndatapp_core)
target_compile_features(anndatapp_core PUBLIC cxx_std_20)
set_target_properties(anndatapp_core PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME core_binary)
target_sources(anndatapp_core
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES
    src/anndatapp.core.cppm src/anndatapp.arrays.cppm src/anndatapp.strings.cppm
    src/anndatapp.records.cppm src/anndatapp.data.cppm
  PRIVATE src/arrays.cpp src/strings.cpp src/records.cpp src/data.cpp)

add_library(anndatapp)
add_library(anndatapp::anndatapp ALIAS anndatapp)
target_link_libraries(anndatapp PUBLIC anndatapp_core)
target_compile_features(anndatapp PUBLIC cxx_std_20)
target_compile_definitions(anndatapp PUBLIC
  ANNDATAPP_HAS_HDF5=$<BOOL:${ANNDATAPP_ENABLE_HDF5}>
  ANNDATAPP_HAS_ZARR=$<BOOL:${ANNDATAPP_ENABLE_ZARR}>
  ANNDATAPP_HAS_PARQUET=$<BOOL:${ANNDATAPP_ENABLE_PARQUET}>)
set_target_properties(anndatapp PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME anndatapp_binary)
target_sources(anndatapp
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES
    src/anndatapp.cppm src/anndata.cppm src/anndatapp.backed.cppm src/anndatapp.streaming.cppm
    src/anndatapp.mudata.cppm src/anndatapp.treedata.cppm
    src/anndatapp.dataset.cppm src/anndatapp.spatialdata.cppm
  PRIVATE src/backed.cpp src/backed_array.cpp src/backed_codec.cpp src/backed_structure.cpp src/backed_writer.cpp
    src/backed_index.cpp src/backed_copy.cpp src/backed_category.cpp src/backed_colors.cpp
    src/backed_records.cpp src/backed_geometry.cpp src/streaming.cpp
    src/store.cpp src/mudata.cpp src/treedata.cpp src/treedata_io.cpp src/dataset.cpp
    src/spatialdata.cpp src/spatialdata_transform.cpp src/spatialdata_io.cpp
    src/spatialdata_parquet.cpp)
target_sources(anndatapp PRIVATE FILE_SET backed_internal TYPE CXX_MODULES
  BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES src/anndatapp.backed.storage.cppm)
if(ANNDATAPP_ENABLE_HDF5)
  target_sources(anndatapp
    PUBLIC FILE_SET CXX_MODULES FILES src/anndatapp.h5ad.cppm
    PRIVATE src/h5ad.cpp src/backed_hdf5.cpp)
  target_link_libraries(anndatapp PRIVATE PkgConfig::ANNDATAPP_HDF5)
endif()
if(ANNDATAPP_ENABLE_HDF5 OR ANNDATAPP_ENABLE_ZARR)
  target_link_libraries(anndatapp PRIVATE "$<BUILD_INTERFACE:nlohmann_json::nlohmann_json>")
endif()
if(ANNDATAPP_ENABLE_PARQUET)
  target_link_libraries(anndatapp PRIVATE ArrowParquet::ArrowParquet)
  # Private shared dependencies also need to resolve from installed libraries.
  set_property(TARGET anndatapp APPEND PROPERTY INSTALL_RPATH ${ArrowParquet_RUNTIME_LIBRARY_DIRS})
endif()
if(ANNDATAPP_ENABLE_ZARR)
  target_sources(anndatapp PUBLIC FILE_SET CXX_MODULES FILES src/anndatapp.zarr.cppm
    PRIVATE src/zarr.cpp src/zarr_engine.cpp src/zarr_codecs.cpp src/zarr_sharding.cpp src/backed_zarr.cpp)
  target_link_libraries(anndatapp PRIVATE PkgConfig::ANNDATAPP_ZLIB PkgConfig::ANNDATAPP_ZSTD Blosc::Blosc
    "$<BUILD_INTERFACE:nlohmann_json::nlohmann_json>")
endif()
target_compile_options(anndatapp_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(anndatapp PRIVATE -Wall -Wextra -Wpedantic)
if(ANNDATAPP_ENABLE_SANITIZERS)
  target_compile_options(anndatapp_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(anndatapp_core PUBLIC -fsanitize=address,undefined)
  target_compile_options(anndatapp PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(anndatapp PUBLIC -fsanitize=address,undefined)
endif()

install(TARGETS anndatapp_core EXPORT anndatappCoreTargets
  FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_DATADIR}/anndatapp/modules
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT anndatappCoreTargets NAMESPACE anndatapp:: CXX_MODULES_DIRECTORY core-cxx-modules
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/anndatapp)
install(TARGETS anndatapp EXPORT anndatappTargets
  FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_DATADIR}/anndatapp/modules
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT anndatappTargets NAMESPACE anndatapp:: CXX_MODULES_DIRECTORY cxx-modules
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/anndatapp)
configure_package_config_file(cmake/anndatappConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/anndatappConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/anndatapp)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/anndatappConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/anndatappConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/anndatappConfigVersion.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindBlosc.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindArrowParquet.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/anndatappModuleProvider.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/anndatapp)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/anndatapp)

if(ANNDATAPP_BUILD_EXAMPLES)
  add_executable(anndatapp_example examples/foundation.cpp)
  target_link_libraries(anndatapp_example PRIVATE anndatapp::anndatapp)
  add_executable(anndatapp_format_family_example examples/format_family.cpp)
  target_link_libraries(anndatapp_format_family_example PRIVATE anndatapp::anndatapp)
endif()
if(ANNDATAPP_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)
  foreach(_suite arrays data strings records copy_into copy_footprint checked_views sequence mudata treedata spatialdata spatialdata_transform)
    add_executable(test_${_suite} tests/test_${_suite}.cpp)
    target_link_libraries(test_${_suite} PRIVATE anndatapp::anndatapp)
    add_test(NAME ${_suite} COMMAND test_${_suite})
  endforeach()
  add_executable(test_spatialdata_wkb tests/test_spatialdata_wkb.cpp)
  target_compile_features(test_spatialdata_wkb PRIVATE cxx_std_20)
  add_test(NAME spatialdata_wkb COMMAND test_spatialdata_wkb)
  if(ANNDATAPP_ENABLE_HDF5 OR ANNDATAPP_ENABLE_ZARR)
    add_executable(test_treedata_json tests/test_treedata_json.cpp)
    target_compile_features(test_treedata_json PRIVATE cxx_std_20)
    target_link_libraries(test_treedata_json PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME treedata_json COMMAND test_treedata_json)
  endif()
  foreach(_suite geometry transfer_context streaming store)
    add_executable(test_${_suite} tests/test_${_suite}.cpp)
    target_link_libraries(test_${_suite} PRIVATE anndatapp::anndatapp Threads::Threads)
    add_test(NAME ${_suite} COMMAND test_${_suite} "${CMAKE_CURRENT_BINARY_DIR}/${_suite}-tests")
  endforeach()
  add_executable(test_backed tests/test_backed.cpp)
  target_link_libraries(test_backed PRIVATE anndatapp::anndatapp)
  if(ANNDATAPP_ENABLE_HDF5)
    target_link_libraries(test_backed PRIVATE PkgConfig::ANNDATAPP_HDF5)
    target_compile_definitions(test_backed PRIVATE ANNDATAPP_TEST_HDF5=1)
  endif()
  add_test(NAME backed COMMAND test_backed "${CMAKE_CURRENT_BINARY_DIR}/backed-tests")
  add_executable(test_backed_categories tests/test_backed_categories.cpp)
  target_link_libraries(test_backed_categories PRIVATE anndatapp::anndatapp)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(test_backed_categories PRIVATE -Wl,--export-dynamic-symbol=pread)
  endif()
  add_test(NAME backed_categories COMMAND test_backed_categories "${CMAKE_CURRENT_BINARY_DIR}/backed-category-tests")
  if(ANNDATAPP_ENABLE_ZARR)
    add_executable(test_backed_revision tests/test_backed_revision.cpp)
    target_link_libraries(test_backed_revision PRIVATE anndatapp::anndatapp)
    add_test(NAME backed_revision COMMAND test_backed_revision "${CMAKE_CURRENT_BINARY_DIR}/backed-revision-tests")
    add_executable(test_zarr_unicode tests/test_zarr_unicode.cpp)
    target_include_directories(test_zarr_unicode PRIVATE src)
    target_link_libraries(test_zarr_unicode PRIVATE anndatapp::anndatapp nlohmann_json::nlohmann_json PkgConfig::ANNDATAPP_ZLIB)
    add_test(NAME zarr_unicode COMMAND test_zarr_unicode)
    add_executable(test_gather_transfer tests/test_gather_transfer.cpp)
    target_include_directories(test_gather_transfer PRIVATE src)
    target_link_libraries(test_gather_transfer PRIVATE anndatapp::anndatapp nlohmann_json::nlohmann_json)
    add_test(NAME gather_transfer COMMAND test_gather_transfer "${CMAKE_CURRENT_BINARY_DIR}/gather-transfer-tests")
    add_executable(test_shard_selection tests/test_shard_selection.cpp)
    target_include_directories(test_shard_selection PRIVATE src)
    target_link_libraries(test_shard_selection PRIVATE anndatapp::anndatapp nlohmann_json::nlohmann_json)
    add_test(NAME shard_selection COMMAND test_shard_selection "${CMAKE_CURRENT_BINARY_DIR}/shard-selection-tests")
    add_executable(test_zarr_sharding tests/test_zarr_sharding.cpp)
    target_include_directories(test_zarr_sharding PRIVATE src)
    target_link_libraries(test_zarr_sharding PRIVATE anndatapp::anndatapp nlohmann_json::nlohmann_json)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_options(test_zarr_sharding PRIVATE
        -Wl,--export-dynamic-symbol=pread -Wl,--export-dynamic-symbol=pwrite)
    endif()
    add_test(NAME zarr_sharding COMMAND test_zarr_sharding "${CMAKE_CURRENT_BINARY_DIR}/shard-tests")
    add_executable(test_record_zarr_rejection tests/test_record_zarr_rejection.cpp)
    target_link_libraries(test_record_zarr_rejection PRIVATE anndatapp::anndatapp)
    add_test(NAME record_zarr_rejection COMMAND test_record_zarr_rejection "${CMAKE_CURRENT_BINARY_DIR}/record-zarr-rejection")
  endif()
  if(ANNDATAPP_ENABLE_HDF5)
    add_test(NAME mudata_lazy COMMAND test_mudata --lazy "${CMAKE_CURRENT_BINARY_DIR}/lazy-mudata.h5mu")
    add_executable(test_bounded_sparse_export tests/test_bounded_sparse_export.cpp)
    target_link_libraries(test_bounded_sparse_export PRIVATE anndatapp::anndatapp)
    add_test(NAME bounded_sparse_export COMMAND test_bounded_sparse_export "${CMAKE_CURRENT_BINARY_DIR}/bounded-sparse.h5mu")
    add_executable(test_dataset tests/test_dataset.cpp)
    target_link_libraries(test_dataset PRIVATE anndatapp::anndatapp)
    add_test(NAME dataset COMMAND test_dataset "${CMAKE_CURRENT_BINARY_DIR}/dataset-tests")
    add_executable(test_bounded_index tests/test_bounded_index.cpp)
    target_link_libraries(test_bounded_index PRIVATE anndatapp::anndatapp PkgConfig::ANNDATAPP_HDF5)
    add_test(NAME bounded_index COMMAND test_bounded_index "${CMAKE_CURRENT_BINARY_DIR}/bounded-index-tests")
    add_executable(test_store_hdf5 tests/test_store_hdf5.cpp)
    target_link_libraries(test_store_hdf5 PRIVATE anndatapp::anndatapp PkgConfig::ANNDATAPP_HDF5)
    add_test(NAME store_hdf5 COMMAND test_store_hdf5 "${CMAKE_CURRENT_BINARY_DIR}/store-hdf5-tests")
    add_executable(test_records_h5ad tests/test_records_h5ad.cpp)
    target_include_directories(test_records_h5ad PRIVATE src)
    target_link_libraries(test_records_h5ad PRIVATE anndatapp::anndatapp PkgConfig::ANNDATAPP_HDF5 ${CMAKE_DL_LIBS})
    add_test(NAME records_h5ad COMMAND test_records_h5ad "${CMAKE_CURRENT_BINARY_DIR}/records-h5ad-tests")
    if(APPLE)
      add_library(test_records_h5ad_observer SHARED tests/h5ad_read_observer.cpp)
      target_link_libraries(test_records_h5ad_observer PRIVATE PkgConfig::ANNDATAPP_HDF5)
      add_dependencies(test_records_h5ad test_records_h5ad_observer)
      set_tests_properties(records_h5ad PROPERTIES
        ENVIRONMENT "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:test_records_h5ad_observer>")
    endif()
    add_executable(test_backed_records tests/test_backed_records.cpp)
    target_link_libraries(test_backed_records PRIVATE anndatapp::anndatapp)
    add_test(NAME backed_records COMMAND test_backed_records "${CMAKE_CURRENT_BINARY_DIR}/backed-record-tests")
    add_executable(test_h5ad tests/test_h5ad.cpp)
    target_link_libraries(test_h5ad PRIVATE anndatapp::anndatapp)
    add_test(NAME h5ad COMMAND test_h5ad "${CMAKE_CURRENT_BINARY_DIR}/h5ad-tests")
    add_executable(test_backed_h5ad tests/test_backed_h5ad.cpp)
    target_link_libraries(test_backed_h5ad PRIVATE anndatapp::anndatapp)
    add_test(NAME backed_h5ad COMMAND test_backed_h5ad "${CMAKE_CURRENT_BINARY_DIR}/h5ad-backed-tests")
  endif()
  if(ANNDATAPP_BUILD_EXAMPLES)
    add_test(NAME import_only_example COMMAND anndatapp_example)
    add_test(NAME format_family_example COMMAND anndatapp_format_family_example)
  endif()
  if(ANNDATAPP_ENABLE_PARQUET)
    add_executable(test_spatialdata_parquet tests/test_spatialdata_parquet.cpp tests/test_spatialdata_parquet_main.cpp)
    target_include_directories(test_spatialdata_parquet PRIVATE src)
    target_link_libraries(test_spatialdata_parquet PRIVATE anndatapp::anndatapp)
    add_test(NAME spatialdata_parquet COMMAND test_spatialdata_parquet "${CMAKE_CURRENT_BINARY_DIR}/spatial-parquet-native")
  endif()
  foreach(_suite sequence mudata treedata spatialdata spatialdata_transform spatialdata_wkb
      treedata_json store store_hdf5 dataset zarr_unicode spatialdata_parquet bounded_sparse_export bounded_index backed_revision checked_views copy_footprint)
    if(TARGET test_${_suite})
      target_compile_options(test_${_suite} PRIVATE -UNDEBUG)
    endif()
  endforeach()
  if(ANNDATAPP_ENABLE_SANITIZERS)
    foreach(_suite spatialdata_wkb treedata_json)
      if(TARGET test_${_suite})
        target_compile_options(test_${_suite} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(test_${_suite} PRIVATE -fsanitize=address,undefined)
      endif()
    endforeach()
  endif()
  add_test(NAME installed_consumer COMMAND ${CMAKE_COMMAND}
    -DANNDATAPP_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -DANNDATAPP_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DANNDATAPP_COMPILER=${CMAKE_CXX_COMPILER}
    -DANNDATAPP_CONFIG=$<CONFIG>
    -DANNDATAPP_HDF5_ENABLED=${ANNDATAPP_ENABLE_HDF5}
    -DANNDATAPP_ZARR_ENABLED=${ANNDATAPP_ENABLE_ZARR}
    -DANNDATAPP_ARROW_ROOT=${ANNDATAPP_ARROW_ROOT}
    -DANNDATAPP_DEPENDENCY_PREFIX=${ANNDATAPP_DEPENDENCY_PREFIX}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test-installed-consumer.cmake)
endif()
if(ANNDATAPP_ENABLE_INTEROP_TESTS)
  enable_testing()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(interop_driver tests/interop_driver.cpp)
  target_link_libraries(interop_driver PRIVATE anndatapp::anndatapp)
  add_test(NAME python_semantics COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_semantics.py $<TARGET_FILE:interop_driver>)
  if(ANNDATAPP_ENABLE_HDF5)
    if(ANNDATAPP_ENABLE_RECORD_INTEROP_TESTS)
      add_executable(records_interop_driver tests/records_interop_driver.cpp)
      target_link_libraries(records_interop_driver PRIVATE anndatapp::anndatapp)
      add_test(NAME python_records COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_records.py $<TARGET_FILE:records_interop_driver>)
    endif()
    add_executable(h5ad_interop_driver tests/h5ad_interop_driver.cpp)
    target_link_libraries(h5ad_interop_driver PRIVATE anndatapp::anndatapp)
    add_test(NAME python_h5ad COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_h5ad.py $<TARGET_FILE:h5ad_interop_driver>)
  endif()
  if(ANNDATAPP_ENABLE_HDF5 AND ANNDATAPP_ENABLE_ZARR)
    find_package(Threads REQUIRED)
    add_executable(streaming_interop_driver tests/streaming_interop_driver.cpp)
    target_link_libraries(streaming_interop_driver PRIVATE anndatapp::anndatapp Threads::Threads)
    add_test(NAME python_streaming COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_streaming.py $<TARGET_FILE:streaming_interop_driver>
      "${CMAKE_CURRENT_BINARY_DIR}/streaming-interop-artifacts")
    add_test(NAME python_streaming_threaded COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_streaming.py $<TARGET_FILE:streaming_interop_driver>
      "${CMAKE_CURRENT_BINARY_DIR}/streaming-threaded-artifacts" --threaded)
    add_executable(completeness_interop_driver tests/completeness_interop_driver.cpp)
    target_link_libraries(completeness_interop_driver PRIVATE anndatapp::anndatapp Threads::Threads)
    add_test(NAME python_completeness COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_completeness.py $<TARGET_FILE:completeness_interop_driver>
      "${CMAKE_CURRENT_BINARY_DIR}/completeness-interop-artifacts")
    add_test(NAME python_completeness_threaded COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_completeness.py $<TARGET_FILE:completeness_interop_driver>
      "${CMAKE_CURRENT_BINARY_DIR}/completeness-threaded-artifacts" --threaded)
    add_executable(storage_interop_driver tests/storage_interop_driver.cpp)
    target_link_libraries(storage_interop_driver PRIVATE anndatapp::anndatapp)
    add_test(NAME python_storage COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_storage.py $<TARGET_FILE:storage_interop_driver>)
  endif()
  if(ANNDATAPP_ENABLE_HDF5 OR ANNDATAPP_ENABLE_ZARR)
    add_executable(dtype_rejection_driver tests/dtype_rejection_driver.cpp)
    target_link_libraries(dtype_rejection_driver PRIVATE anndatapp::anndatapp)
    if(ANNDATAPP_ENABLE_HDF5)
      add_test(NAME python_unsupported_dtype_h5ad COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_unsupported_dtype.py
        $<TARGET_FILE:dtype_rejection_driver> h5ad
        "${CMAKE_CURRENT_BINARY_DIR}/unsupported-dtype-h5ad")
    endif()
    if(ANNDATAPP_ENABLE_ZARR)
      foreach(_format zarr2 zarr3)
        add_test(NAME python_unsupported_dtype_${_format} COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_unsupported_dtype.py
          $<TARGET_FILE:dtype_rejection_driver> ${_format}
          "${CMAKE_CURRENT_BINARY_DIR}/unsupported-dtype-${_format}")
      endforeach()
    endif()
  endif()
endif()
if(ANNDATAPP_ENABLE_FORMAT_INTEROP_TESTS)
  enable_testing()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(ANNDATAPP_TREEDATA_LEGACY_PYTHON "" CACHE FILEPATH "Optional pinned TreeData 0.2.5 reference interpreter")
  set(ANNDATAPP_SPATIAL_LEGACY02_STORE "" CACHE PATH "Pinned SpatialData storage 0.2 reference fixture")
  set(ANNDATAPP_SPATIAL_LEGACY03_STORE "" CACHE PATH "Pinned SpatialData storage 0.3 reference fixture")
  if(ANNDATAPP_ENABLE_HDF5 AND ANNDATAPP_ENABLE_ZARR)
    add_test(NAME python_mudata COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_mudata.py $<TARGET_FILE:test_mudata>
      "${CMAKE_CURRENT_BINARY_DIR}/mudata-interop")
    set_tests_properties(python_mudata PROPERTIES ENVIRONMENT "ANNDATAPP_INTEROP_ZARR_WAKEUP=1")
    set(_tree_legacy)
    if(ANNDATAPP_TREEDATA_LEGACY_PYTHON)
      list(APPEND _tree_legacy --legacy-python "${ANNDATAPP_TREEDATA_LEGACY_PYTHON}")
    endif()
    add_test(NAME python_treedata COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_treedata_roundtrip.py $<TARGET_FILE:test_treedata> ${_tree_legacy})
  endif()
  if(ANNDATAPP_ENABLE_HDF5)
    add_test(NAME python_dataset COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_dataset_roundtrip.py $<TARGET_FILE:test_dataset>)
  endif()
  if(ANNDATAPP_ENABLE_ZARR AND ANNDATAPP_ENABLE_PARQUET)
    set(_spatial_legacy)
    if(ANNDATAPP_SPATIAL_LEGACY02_STORE)
      list(APPEND _spatial_legacy --legacy02 "${ANNDATAPP_SPATIAL_LEGACY02_STORE}")
    endif()
    if(ANNDATAPP_SPATIAL_LEGACY03_STORE)
      list(APPEND _spatial_legacy --legacy03 "${ANNDATAPP_SPATIAL_LEGACY03_STORE}")
    endif()
    add_test(NAME python_spatialdata COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_spatialdata_roundtrip.py $<TARGET_FILE:test_spatialdata>
      "${CMAKE_CURRENT_BINARY_DIR}/spatialdata-interop" ${_spatial_legacy})
    set_tests_properties(python_spatialdata PROPERTIES ENVIRONMENT "ANNDATAPP_INTEROP_ZARR_WAKEUP=1")
    add_test(NAME python_spatialdata_parquet COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/spatialdata_parquet_reference.py $<TARGET_FILE:test_spatialdata_parquet>
      "${CMAKE_CURRENT_BINARY_DIR}/spatial-parquet-interop")
  endif()
endif()
if(ANNDATAPP_BUILD_BENCHMARKS)
  find_package(Threads REQUIRED)
  if(ANNDATAPP_ENABLE_ZARR)
    add_executable(completeness_probe benchmarks/completeness.cpp)
    target_link_libraries(completeness_probe PRIVATE anndatapp::anndatapp ${CMAKE_DL_LIBS})
    add_executable(completeness_adaptive benchmarks/completeness_adaptive.cpp)
    target_link_libraries(completeness_adaptive PRIVATE anndatapp::anndatapp Threads::Threads ${CMAKE_DL_LIBS})
  endif()
  add_executable(streaming_benchmark benchmarks/streaming.cpp)
  target_compile_definitions(streaming_benchmark PRIVATE ANNDATAPP_STREAM_CANDIDATE=1)
  target_link_libraries(streaming_benchmark PRIVATE anndatapp::anndatapp Threads::Threads)
  add_executable(records_benchmark benchmarks/records.cpp)
  target_link_libraries(records_benchmark PRIVATE anndatapp::anndatapp)
  add_executable(anndatapp_selection_probe benchmarks/selection.cpp)
  target_link_libraries(anndatapp_selection_probe PRIVATE anndatapp::anndatapp)
  add_executable(memory_benchmark benchmarks/memory.cpp)
  target_link_libraries(memory_benchmark PRIVATE anndatapp::anndatapp Threads::Threads)
  if(ANNDATAPP_ENABLE_HDF5)
    add_executable(record_io_probe benchmarks/record_io_probe/driver.cpp)
    target_include_directories(record_io_probe PRIVATE benchmarks/record_io_probe)
    target_link_libraries(record_io_probe PRIVATE anndatapp::anndatapp ${CMAKE_DL_LIBS})
    add_executable(h5ad_benchmark benchmarks/h5ad.cpp)
    target_link_libraries(h5ad_benchmark PRIVATE anndatapp::anndatapp)
  endif()
  if(ANNDATAPP_ENABLE_HDF5 AND ANNDATAPP_ENABLE_ZARR)
    add_executable(storage_benchmark benchmarks/storage.cpp)
    target_link_libraries(storage_benchmark PRIVATE anndatapp::anndatapp Threads::Threads)
  endif()
endif()
