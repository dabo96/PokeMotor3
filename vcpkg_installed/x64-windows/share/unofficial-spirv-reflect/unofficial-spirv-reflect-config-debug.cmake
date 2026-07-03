#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "unofficial::spirv-reflect" for configuration "Debug"
set_property(TARGET unofficial::spirv-reflect APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(unofficial::spirv-reflect PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/lib/spirv-reflect-static.lib"
  )

list(APPEND _cmake_import_check_targets unofficial::spirv-reflect )
list(APPEND _cmake_import_check_files_for_unofficial::spirv-reflect "${_IMPORT_PREFIX}/debug/lib/spirv-reflect-static.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
