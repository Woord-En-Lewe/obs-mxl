cmake_minimum_required(VERSION 3.24)

include(FetchContent)

find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(Catch2)
endif()

add_executable(obsmxl_tests
  test_discovery.cpp
  test_domain_options.cpp
  test_egress.cpp
  test_fake_backend.cpp
  test_flow_definition.cpp
  test_ingress.cpp
  test_json.cpp
  test_roundtrip.cpp
  test_settings.cpp
  test_time.cpp
  test_uuid.cpp
  test_v210.cpp
)

target_link_libraries(obsmxl_tests PRIVATE obsmxl_core Catch2::Catch2WithMain)
target_include_directories(obsmxl_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
obsmxl_apply_target_defaults(obsmxl_tests)

if(OBSMXL_HAVE_LIBMXL)
  target_sources(obsmxl_tests PRIVATE test_mxl_integration.cpp)
  target_link_libraries(obsmxl_tests PRIVATE obsmxl_backend_mxl)
  target_compile_definitions(obsmxl_tests PRIVATE OBSMXL_HAVE_LIBMXL=1)
endif()

include(Catch)
catch_discover_tests(obsmxl_tests)
