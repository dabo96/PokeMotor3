#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "vk-bootstrap::vk-bootstrap" for configuration "Debug"
set_property(TARGET vk-bootstrap::vk-bootstrap APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(vk-bootstrap::vk-bootstrap PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/lib/vk-bootstrap.lib"
  )

list(APPEND _cmake_import_check_targets vk-bootstrap::vk-bootstrap )
list(APPEND _cmake_import_check_files_for_vk-bootstrap::vk-bootstrap "${_IMPORT_PREFIX}/debug/lib/vk-bootstrap.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
