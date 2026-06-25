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
