#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "vk-bootstrap::vk-bootstrap" for configuration "Release"
set_property(TARGET vk-bootstrap::vk-bootstrap APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(vk-bootstrap::vk-bootstrap PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/vk-bootstrap.lib"
  )

list(APPEND _cmake_import_check_targets vk-bootstrap::vk-bootstrap )
list(APPEND _cmake_import_check_files_for_vk-bootstrap::vk-bootstrap "${_IMPORT_PREFIX}/lib/vk-bootstrap.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
