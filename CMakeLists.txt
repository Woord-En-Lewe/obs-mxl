cmake_minimum_required(VERSION 3.24)

project(
  obs-mxl
  VERSION 1.1.0
  DESCRIPTION "Unified EBU MXL ingress/egress plugin for OBS Studio"
  LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(ObsMxlHelpers)

find_package(Threads REQUIRED)

option(OBS_MXL_ENABLE_PLUGIN "Build the OBS Studio plugin module" ON)
option(OBS_MXL_ENABLE_UI "Build the Qt based configuration dialog" ON)
option(OBS_MXL_BUILD_TESTS "Build the unit tests" ON)
option(OBS_MXL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(OBS_MXL_ENABLE_SANITIZERS "Enable address/undefined sanitizers" OFF)

set(OBSMXL_HAVE_UI OFF)
set(OBSMXL_HAVE_LIBMXL OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/obsmxl/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/obsmxl/version.hpp"
  @ONLY
)

set(OBSMXL_PUBLIC_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

# ---------------------------------------------------------------------------
# Core: host agnostic timing, flow definitions, conversions and pipelines.
# This target deliberately has no OBS, Qt or libmxl dependency so it can be
# built and tested on macOS (the development platform) and in Linux CI.
# ---------------------------------------------------------------------------
add_library(obsmxl_core STATIC
  src/core/backend-fake.cpp
  src/core/discovery.cpp
  src/core/domain-options.cpp
  src/core/flow-definition.cpp
  src/core/ingress.cpp
  src/core/egress.cpp
  src/core/egress-audio.cpp
  src/core/json.cpp
  src/core/log.cpp
  src/core/settings.cpp
  src/core/time.cpp
  src/core/uuid.cpp
  src/core/v210.cpp
)
add_library(obsmxl::core ALIAS obsmxl_core)

target_include_directories(obsmxl_core PUBLIC ${OBSMXL_PUBLIC_INCLUDE_DIRS})
target_link_libraries(obsmxl_core PUBLIC Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(obsmxl_core PRIVATE rt)
endif()

obsmxl_apply_target_defaults(obsmxl_core)

# ---------------------------------------------------------------------------
# Real libmxl backend. Only built when the MXL SDK is available.
# ---------------------------------------------------------------------------
set(OBSMXL_HAVE_LIBMXL OFF)
find_package(mxl CONFIG QUIET)
if(mxl_FOUND OR TARGET mxl::mxl)
  set(OBSMXL_HAVE_LIBMXL ON)

  add_library(obsmxl_backend_mxl STATIC src/mxl/mxl-backend.cpp)
  add_library(obsmxl::backend_mxl ALIAS obsmxl_backend_mxl)
  target_link_libraries(obsmxl_backend_mxl PUBLIC obsmxl_core)
  if(TARGET mxl::mxl)
    target_link_libraries(obsmxl_backend_mxl PRIVATE mxl::mxl)
  else()
    target_link_libraries(obsmxl_backend_mxl PRIVATE mxl)
  endif()
  target_compile_definitions(obsmxl_backend_mxl PUBLIC OBSMXL_HAVE_LIBMXL=1)
  obsmxl_apply_target_defaults(obsmxl_backend_mxl)
  message(STATUS "obs-mxl: MXL SDK found, real backend enabled")
else()
  message(STATUS "obs-mxl: MXL SDK not found, real backend disabled (core/tests only)")
endif()

# ---------------------------------------------------------------------------
# OBS plugin module
# ---------------------------------------------------------------------------
set(OBSMXL_PLUGIN_ENABLED OFF)

if(OBS_MXL_ENABLE_PLUGIN)
  find_package(libobs CONFIG QUIET)
  if(TARGET libobs AND NOT TARGET OBS::libobs)
    add_library(OBS::libobs ALIAS libobs)
  endif()
  if(NOT TARGET OBS::libobs)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(LIBOBS IMPORTED_TARGET libobs)
      if(LIBOBS_FOUND)
        add_library(OBS::libobs ALIAS PkgConfig::LIBOBS)
      endif()
    endif()
  endif()
  if(NOT TARGET OBS::libobs)
    find_library(OBS_LIBRARY NAMES obs libobs)
    if(OBS_LIBRARY)
      add_library(OBS::libobs UNKNOWN IMPORTED)
      set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "${OBS_LIBRARY}")
    endif()
  endif()

  if(TARGET OBS::libobs)
    find_package(obs-frontend-api CONFIG QUIET)
    if(TARGET OBS::obs-frontend-api AND NOT TARGET OBS::frontend-api)
      add_library(OBS::frontend-api ALIAS OBS::obs-frontend-api)
    endif()
    if(TARGET obs-frontend-api AND NOT TARGET OBS::frontend-api)
      add_library(OBS::frontend-api ALIAS obs-frontend-api)
    endif()
    if(NOT TARGET OBS::frontend-api)
      find_library(OBS_FRONTEND_API_LIBRARY NAMES obs-frontend-api libobs-frontend-api)
      if(OBS_FRONTEND_API_LIBRARY)
        add_library(OBS::frontend-api UNKNOWN IMPORTED)
        set_target_properties(OBS::frontend-api PROPERTIES
          IMPORTED_LOCATION "${OBS_FRONTEND_API_LIBRARY}")
      endif()
    endif()

    set(OBSMXL_HAVE_UI OFF)
    if(OBS_MXL_ENABLE_UI)
      find_package(Qt6 6.2 COMPONENTS Widgets QUIET)
      if(Qt6_FOUND)
        set(OBSMXL_HAVE_UI ON)
      endif()
    endif()

    if(NOT OBSMXL_HAVE_LIBMXL)
      message(WARNING
        "obs-mxl: the OBS plugin requires the MXL SDK. "
        "Install libmxl (see docs/building.md) or disable OBS_MXL_ENABLE_PLUGIN.")
    else()
      set(OBSMXL_PLUGIN_ENABLED ON)
    endif()
  elseif(APPLE)
    message(STATUS
      "obs-mxl: libobs not found; skipping the plugin module (macOS core-only build). "
      "See docs/building.md for full plugin builds.")
  else()
    message(FATAL_ERROR
      "obs-mxl: libobs was not found. Install the OBS development headers "
      "(libobs-dev on Debian/Ubuntu) or pass -DOBS_MXL_ENABLE_PLUGIN=OFF.")
  endif()
endif()

if(OBSMXL_PLUGIN_ENABLED)
  if(OBSMXL_HAVE_UI)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
  endif()

  add_library(obs-mxl MODULE
    src/obs/mxl-output.cpp
    src/obs/mxl-source.cpp
    src/obs/obs-config-store.cpp
    src/obs/obs-log.cpp
    src/obs/output-manager.cpp
    src/obs/plugin-main.cpp
    src/obs/preview-renderer.cpp
  )
  target_include_directories(obs-mxl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  if(NOT TARGET OBS::libobs)
    find_path(OBS_INCLUDE_DIR NAMES obs.h PATH_SUFFIXES obs)
    if(OBS_INCLUDE_DIR)
      target_include_directories(obs-mxl PRIVATE "${OBS_INCLUDE_DIR}")
    endif()
  endif()
  target_link_libraries(obs-mxl PRIVATE obsmxl_core obsmxl_backend_mxl)

  if(TARGET OBS::libobs)
    target_link_libraries(obs-mxl PRIVATE OBS::libobs)
  else()
    target_link_libraries(obs-mxl PRIVATE obs)
  endif()

  if(TARGET OBS::frontend-api)
    target_link_libraries(obs-mxl PRIVATE OBS::frontend-api)
  elseif(TARGET obs-frontend-api)
    target_link_libraries(obs-mxl PRIVATE obs-frontend-api)
  endif()

  if(OBSMXL_HAVE_UI)
    target_sources(obs-mxl PRIVATE
      src/ui/mxl-output-dialog.cpp
      src/ui/mxl-output-dialog.hpp
      src/ui/mxl-output-dialog.ui
    )
    target_link_libraries(obs-mxl PRIVATE Qt6::Widgets)
    target_compile_definitions(obs-mxl PRIVATE OBSMXL_HAVE_UI=1)
  endif()

  set_target_properties(obs-mxl PROPERTIES
    PREFIX ""
    OUTPUT_NAME "obs-mxl"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
  obsmxl_apply_target_defaults(obs-mxl)

  include(ObsMxlPackaging)
  obsmxl_install_plugin(obs-mxl)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(OBS_MXL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

obsmxl_print_summary()
