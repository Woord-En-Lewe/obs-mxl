include_guard(GLOBAL)

function(obsmxl_apply_target_defaults target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(OBS_MXL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wshadow
      -Wnon-virtual-dtor
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
    )
    if(OBS_MXL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(OBS_MXL_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

function(obsmxl_print_summary)
  message(STATUS "")
  message(STATUS "obs-mxl ${PROJECT_VERSION} configuration")
  message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
  message(STATUS "  plugin module ...... ${OBSMXL_PLUGIN_ENABLED}")
  message(STATUS "  Qt dialog .......... ${OBSMXL_HAVE_UI}")
  message(STATUS "  real MXL backend ... ${OBSMXL_HAVE_LIBMXL}")
  message(STATUS "  tests .............. ${OBS_MXL_BUILD_TESTS}")
  message(STATUS "")
endfunction()
