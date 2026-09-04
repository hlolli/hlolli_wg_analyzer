set(_ONNXRuntime_ROOT_HINTS)
set(_ONNXRuntime_SEARCH_OPTIONS)
if(ONNXRuntime_ROOT)
  set(_ONNXRuntime_ROOT "${ONNXRuntime_ROOT}")
elseif(NOT "$ENV{ONNXRuntime_ROOT}" STREQUAL "")
  set(_ONNXRuntime_ROOT "$ENV{ONNXRuntime_ROOT}")
endif()
if(_ONNXRuntime_ROOT)
  if(NOT IS_ABSOLUTE "${_ONNXRuntime_ROOT}")
    message(FATAL_ERROR
      "ONNXRuntime_ROOT must be an absolute path: ${_ONNXRuntime_ROOT}")
  endif()
  unset(ONNXRuntime_INCLUDE_DIR CACHE)
  unset(ONNXRuntime_LIBRARY CACHE)
  unset(ONNXRuntime_RUNTIME_LIBRARY CACHE)
  list(APPEND _ONNXRuntime_ROOT_HINTS "${_ONNXRuntime_ROOT}")
  list(APPEND _ONNXRuntime_SEARCH_OPTIONS NO_DEFAULT_PATH)
endif()

find_path(ONNXRuntime_INCLUDE_DIR
  NAMES onnxruntime_c_api.h
  HINTS ${_ONNXRuntime_ROOT_HINTS}
  PATH_SUFFIXES
    include
    include/onnxruntime
    include/onnxruntime/core/session
  ${_ONNXRuntime_SEARCH_OPTIONS}
)

find_library(ONNXRuntime_LIBRARY
  NAMES onnxruntime
  HINTS ${_ONNXRuntime_ROOT_HINTS}
  PATH_SUFFIXES
    lib
    lib64
  ${_ONNXRuntime_SEARCH_OPTIONS}
)

if(WIN32)
  find_file(ONNXRuntime_RUNTIME_LIBRARY
    NAMES onnxruntime.dll
    HINTS ${_ONNXRuntime_ROOT_HINTS}
    PATH_SUFFIXES
      bin
      lib
      lib64
    ${_ONNXRuntime_SEARCH_OPTIONS}
  )
endif()

foreach(_ONNXRuntime_PATH_VARIABLE IN ITEMS
    ONNXRuntime_INCLUDE_DIR
    ONNXRuntime_LIBRARY
    ONNXRuntime_RUNTIME_LIBRARY)
  if(DEFINED ${_ONNXRuntime_PATH_VARIABLE} AND
      NOT "${${_ONNXRuntime_PATH_VARIABLE}}" STREQUAL "" AND
      NOT "${${_ONNXRuntime_PATH_VARIABLE}}" MATCHES "-NOTFOUND$" AND
      NOT IS_ABSOLUTE "${${_ONNXRuntime_PATH_VARIABLE}}")
    message(FATAL_ERROR
      "${_ONNXRuntime_PATH_VARIABLE} must be an absolute path: "
      "${${_ONNXRuntime_PATH_VARIABLE}}")
  endif()
endforeach()

include(FindPackageHandleStandardArgs)
set(_ONNXRuntime_REQUIRED_VARIABLES
  ONNXRuntime_INCLUDE_DIR
  ONNXRuntime_LIBRARY)
if(WIN32)
  list(APPEND _ONNXRuntime_REQUIRED_VARIABLES
    ONNXRuntime_RUNTIME_LIBRARY)
endif()
find_package_handle_standard_args(ONNXRuntime
  REQUIRED_VARS ${_ONNXRuntime_REQUIRED_VARIABLES}
)

if(ONNXRuntime_FOUND AND NOT TARGET ONNXRuntime::ONNXRuntime)
  if(WIN32)
    add_library(ONNXRuntime::ONNXRuntime SHARED IMPORTED)
    set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
      IMPORTED_IMPLIB "${ONNXRuntime_LIBRARY}"
      IMPORTED_LOCATION "${ONNXRuntime_RUNTIME_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${ONNXRuntime_INCLUDE_DIR}"
    )
  else()
    add_library(ONNXRuntime::ONNXRuntime UNKNOWN IMPORTED)
    set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
      IMPORTED_LOCATION "${ONNXRuntime_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${ONNXRuntime_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(
  ONNXRuntime_INCLUDE_DIR
  ONNXRuntime_LIBRARY
)
if(WIN32)
  mark_as_advanced(ONNXRuntime_RUNTIME_LIBRARY)
endif()

unset(_ONNXRuntime_PATH_VARIABLE)
unset(_ONNXRuntime_REQUIRED_VARIABLES)
unset(_ONNXRuntime_ROOT_HINTS)
unset(_ONNXRuntime_SEARCH_OPTIONS)
unset(_ONNXRuntime_ROOT)
