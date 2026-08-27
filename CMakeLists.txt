cmake_minimum_required(VERSION 3.24)
project(pp-cpp-common VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PP_COMMON_BUILD_TESTS "Build pp-cpp-common unit tests" OFF)

add_library(pp_common STATIC
  src/CivilTime.cpp
  src/Logger.cpp
  src/Module.cpp
  src/SequencedTaskRunner.cpp
  src/Utilities.cpp
  src/Value.cpp
  src/WorkerPool.cpp
  src/io/Json.cpp
)
add_library(pp::common ALIAS pp_common)

target_include_directories(pp_common PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

if(MSVC)
  target_compile_definitions(pp_common PUBLIC NOMINMAX)
endif()

include(GNUInstallDirs)
install(TARGETS pp_common
  EXPORT pp_commonTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT pp_commonTargets
  FILE pp_commonTargets.cmake
  NAMESPACE pp::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pp_common)

if(PP_COMMON_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )
  FetchContent_MakeAvailable(googletest)

  add_executable(pp_common_test
    tests/civil_time_test.cpp
    tests/logger_emit_floor_test.cpp
    tests/value_meta_test.cpp
    tests/worker_pool_test.cpp)
  target_link_libraries(pp_common_test PRIVATE
    pp_common
    GTest::gtest
    GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(pp_common_test)
endif()
