# Conan target bridge for the standalone AEMU tree.
#
# Include this after Conan's generated CMakeDeps files and before AEMU targets
# are declared. The current upstream build defines many short target names from
# checked-in external/* projects. This bridge intentionally exposes only a small
# compatibility layer; delete aliases as upstream CMake is converted to
# package-native targets.

include(CMakeFindDependencyMacro)

find_dependency(absl CONFIG)
find_dependency(flatbuffers CONFIG)
find_dependency(libusb CONFIG)
find_dependency(libxml2 CONFIG)
find_dependency(lz4 CONFIG)
find_dependency(protobuf CONFIG)
find_dependency(ZLIB CONFIG)

function(aemu_collect_conan_runtime_dylibs out_var package)
  set(_configs)
  if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_config)
    list(APPEND _configs "${_build_config}")
  endif()
  list(APPEND _configs RELEASE)
  list(REMOVE_DUPLICATES _configs)

  set(_result)
  foreach(_config IN LISTS _configs)
    set(_type_var "${package}_LIBRARY_TYPE_${_config}")
    set(_dirs_var "${package}_LIB_DIRS_${_config}")
    if(DEFINED ${_type_var} AND DEFINED ${_dirs_var}
       AND "${${_type_var}}" STREQUAL "SHARED")
      foreach(_dir IN LISTS ${_dirs_var})
        foreach(_pattern IN LISTS ARGN)
          file(GLOB _matches "${_dir}/${_pattern}")
          list(APPEND _result ${_matches})
        endforeach()
      endforeach()
      break()
    endif()
  endforeach()

  if(_result)
    list(REMOVE_DUPLICATES _result)
  endif()
  set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

function(aemu_add_conan_runtime_copy_commands out_var)
  set(_outputs)
  set(_seen_names)
  foreach(_src IN LISTS ARGN)
    if(NOT EXISTS "${_src}")
      continue()
    endif()
    # Conan packages often include unversioned symlinks for link-time lookup.
    # Dyld uses the dylib install names at runtime, so only materialize files.
    if(IS_SYMLINK "${_src}")
      continue()
    endif()
    get_filename_component(_name "${_src}" NAME)
    if(_name IN_LIST _seen_names)
      continue()
    endif()
    get_filename_component(_real_src "${_src}" REALPATH)
    set(_dst "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${_name}")
    add_custom_command(
      OUTPUT "${_dst}"
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_real_src}" "${_dst}"
      DEPENDS "${_real_src}"
      COMMENT "Copying Conan runtime ${_name}"
      VERBATIM)
    list(APPEND _outputs "${_dst}")
    list(APPEND _seen_names "${_name}")
  endforeach()
  set(${out_var} "${_outputs}" PARENT_SCOPE)
endfunction()

set(_AEMU_CONAN_RUNTIME_DYLIBS)
if(DARWIN_X86_64 OR DARWIN_AARCH64)
  aemu_collect_conan_runtime_dylibs(_AEMU_PROTOBUF_RUNTIME_DYLIBS protobuf
                                    "libprotobuf*.dylib"
                                    "libprotoc*.dylib"
                                    "libutf8_*.dylib")
  aemu_collect_conan_runtime_dylibs(_AEMU_ABSL_RUNTIME_DYLIBS abseil
                                    "libabsl_*.dylib")
  list(APPEND _AEMU_CONAN_RUNTIME_DYLIBS ${_AEMU_PROTOBUF_RUNTIME_DYLIBS}
       ${_AEMU_ABSL_RUNTIME_DYLIBS})
  if(_AEMU_CONAN_RUNTIME_DYLIBS)
    list(REMOVE_DUPLICATES _AEMU_CONAN_RUNTIME_DYLIBS)
  endif()
endif()

set(_AEMU_CONAN_RUNTIME_LIBRARY_DIRS)
foreach(_dylib IN LISTS _AEMU_CONAN_RUNTIME_DYLIBS)
  get_filename_component(_dylib_dir "${_dylib}" DIRECTORY)
  list(APPEND _AEMU_CONAN_RUNTIME_LIBRARY_DIRS "${_dylib_dir}")
endforeach()
if(_AEMU_CONAN_RUNTIME_LIBRARY_DIRS)
  list(REMOVE_DUPLICATES _AEMU_CONAN_RUNTIME_LIBRARY_DIRS)
  list(JOIN _AEMU_CONAN_RUNTIME_LIBRARY_DIRS ":" _AEMU_CONAN_RUNTIME_LIBRARY_PATH)
  set_property(GLOBAL PROPERTY AEMU_CONAN_RUNTIME_LIBRARY_PATH
                               "${_AEMU_CONAN_RUNTIME_LIBRARY_PATH}")
endif()

aemu_add_conan_runtime_copy_commands(_AEMU_CONAN_RUNTIME_OUTPUTS
                                     ${_AEMU_CONAN_RUNTIME_DYLIBS})
add_custom_target(aemu-conan-shared-runtime
                  DEPENDS ${_AEMU_CONAN_RUNTIME_OUTPUTS})
if(_AEMU_CONAN_RUNTIME_OUTPUTS)
  list(LENGTH _AEMU_CONAN_RUNTIME_OUTPUTS _AEMU_CONAN_RUNTIME_COUNT)
  message(STATUS
          "Conan shared runtime: ${_AEMU_CONAN_RUNTIME_COUNT} dylibs -> ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
  )
  install(FILES ${_AEMU_CONAN_RUNTIME_OUTPUTS} DESTINATION lib64)
endif()

function(aemu_promote_imported_target target)
  if(TARGET "${target}")
    get_target_property(_imported "${target}" IMPORTED)
    if(_imported)
      set_property(TARGET "${target}" PROPERTY IMPORTED_GLOBAL TRUE)
    endif()
  endif()
endfunction()

foreach(_target ${abseil_COMPONENT_NAMES} abseil::abseil flatbuffers::flatbuffers
                flatbuffers::flatc libusb::libusb LibXml2::LibXml2 lz4::lz4
                LZ4::lz4_static protobuf::protobuf protobuf::libprotobuf
                protobuf::libprotoc protobuf::protoc ZLIB::ZLIB)
  aemu_promote_imported_target("${_target}")
endforeach()

function(aemu_alias_if_missing alias target)
  if(TARGET "${target}" AND NOT TARGET "${alias}")
    add_library("${alias}" INTERFACE IMPORTED GLOBAL)
    target_link_libraries("${alias}" INTERFACE "${target}")
  endif()
endfunction()

aemu_alias_if_missing(zlib ZLIB::ZLIB)

set(_AEMU_LZ4_TARGET "")
foreach(_target lz4::lz4 lz4::lz4_static LZ4::lz4_static LZ4::lz4 LZ4::LZ4)
  if(TARGET "${_target}")
    set(_AEMU_LZ4_TARGET "${_target}")
    break()
  endif()
endforeach()
if(NOT _AEMU_LZ4_TARGET)
  message(FATAL_ERROR "Conan lz4 package did not expose a known CMake target")
endif()
aemu_alias_if_missing(lz4 "${_AEMU_LZ4_TARGET}")
aemu_alias_if_missing(flatbuffers flatbuffers::flatbuffers)
aemu_alias_if_missing(emulator-libusb libusb::libusb)

if(TARGET flatbuffers::flatc AND NOT TARGET flatc)
  get_target_property(_AEMU_FLATC_LOCATION flatbuffers::flatc IMPORTED_LOCATION)
  if(NOT _AEMU_FLATC_LOCATION)
    get_target_property(_AEMU_FLATC_LOCATION flatbuffers::flatc IMPORTED_LOCATION_RELEASE)
  endif()
  if(_AEMU_FLATC_LOCATION)
    add_executable(flatc IMPORTED GLOBAL)
    set_target_properties(flatc PROPERTIES IMPORTED_LOCATION
                                           "${_AEMU_FLATC_LOCATION}")
  endif()
endif()

# Some Abseil package revisions export short log target names while this tree
# was written against the names from the checked-in Abseil CMake files.
aemu_alias_if_missing(absl::absl_log absl::log)

# Keep the AOSP names visible during the transition. These are intentionally
# narrow: they should point at the real package targets, not at bundled source.
if(TARGET absl::base AND NOT TARGET abseil-cpp)
  add_library(abseil-cpp INTERFACE IMPORTED)
  target_link_libraries(
    abseil-cpp
    INTERFACE
      absl::base
      absl::strings
      absl::synchronization
      absl::time
      absl::status
      absl::statusor)
endif()
