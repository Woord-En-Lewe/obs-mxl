include_guard(GLOBAL)

# OBS plugin layout:
#   Linux:   <prefix>/lib/obs-plugins/obs-mxl.so
#            <prefix>/share/obs/obs-plugins/obs-mxl/{data,locale}
#   macOS:   ~/Library/Application Support/obs-studio/plugins/obs-mxl.plugin/
#              Contents/MacOS/obs-mxl.so
#              Contents/Resources/{data,locale}
#
# Distributions set OBS_PLUGIN_DESTINATION / OBS_DATA_DESTINATION to match
# their OBS packaging.
set(OBS_PLUGIN_DESTINATION
    "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
    CACHE STRING "Install destination for OBS plugin binaries")
set(OBS_DATA_DESTINATION
    "${CMAKE_INSTALL_DATADIR}/obs/obs-plugins"
    CACHE STRING "Install destination for OBS plugin data")

function(obsmxl_install_plugin target)
  if(APPLE)
    set(plugin_dir "${OBS_PLUGIN_DESTINATION}/obs-mxl.plugin")
    install(TARGETS ${target}
      LIBRARY DESTINATION "${plugin_dir}/Contents/MacOS"
      RUNTIME DESTINATION "${plugin_dir}/Contents/MacOS")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/locale"
      DESTINATION "${plugin_dir}/Contents/Resources")
  else()
    install(TARGETS ${target} LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/locale"
      DESTINATION "${OBS_DATA_DESTINATION}/obs-mxl")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/obs-mxl/README.md"
      DESTINATION "${OBS_DATA_DESTINATION}/obs-mxl" OPTIONAL)
  endif()
endfunction()
