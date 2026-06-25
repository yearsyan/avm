# Copyright 2018 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
# specific language governing permissions and limitations under the License.

get_filename_component(LIBXML2_SOURCE_DIR
                       "${ANDROID_QEMU2_TOP_DIR}/../libxml2" ABSOLUTE)
set(LIBXML2_BINARY_DIR "${CMAKE_BINARY_DIR}/android/third_party/libxml2")

if(NOT TARGET LibXml2::LibXml2 OR
   (NOT libxml2_INCLUDE_DIRS AND NOT libxml2_INCLUDE_DIRS_RELEASE))
  find_package(libxml2 CONFIG QUIET)
endif()

if(NOT TARGET LibXml2::LibXml2)
  set(_AEMU_SAVED_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_LZMA OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_PYTHON OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_TESTS OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_ZLIB OFF CACHE BOOL "" FORCE)

  add_subdirectory("${LIBXML2_SOURCE_DIR}" "${LIBXML2_BINARY_DIR}"
                   EXCLUDE_FROM_ALL)
  target_compile_definitions(LibXml2 PRIVATE "ICONV_CONST=")

  if(DEFINED _AEMU_SAVED_BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS "${_AEMU_SAVED_BUILD_SHARED_LIBS}" CACHE BOOL ""
                                                               FORCE)
  else()
    unset(BUILD_SHARED_LIBS CACHE)
  endif()

endif()

set(_AEMU_LIBXML2_LICENSE_TARGETS LibXml2::LibXml2)
if(TARGET LibXml2)
  list(APPEND _AEMU_LIBXML2_LICENSE_TARGETS LibXml2)
endif()
android_license(
  TARGET ${_AEMU_LIBXML2_LICENSE_TARGETS}
  LIBNAME LibXml2
  URL "https://gitlab.gnome.org/GNOME/libxml2"
  SPDX "MIT"
  LICENSE "https://github.com/GNOME/libxml2/blob/mainline/Copyright"
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.LIBXML2")

if(libxml2_INCLUDE_DIRS)
  set(_AEMU_LIBXML2_INCLUDE_DIRS "${libxml2_INCLUDE_DIRS}")
elseif(libxml2_INCLUDE_DIRS_RELEASE)
  set(_AEMU_LIBXML2_INCLUDE_DIRS "${libxml2_INCLUDE_DIRS_RELEASE}")
else()
  get_target_property(_AEMU_LIBXML2_INCLUDE_DIRS LibXml2::LibXml2
                      INTERFACE_INCLUDE_DIRECTORIES)
endif()
if(NOT _AEMU_LIBXML2_INCLUDE_DIRS)
  set(_AEMU_LIBXML2_INCLUDE_DIRS "")
endif()

set(LIBXML2_INCLUDE_DIRS "${_AEMU_LIBXML2_INCLUDE_DIRS}")
if(LIBXML2_INCLUDE_DIRS)
  list(GET LIBXML2_INCLUDE_DIRS 0 LIBXML2_INCLUDE_DIR)
else()
  set(LIBXML2_INCLUDE_DIR "")
endif()
set(LIBXML2_LIBRARIES LibXml2::LibXml2)
if(NOT LIBXML2_DEFINITIONS)
  set(LIBXML2_DEFINITIONS "-DLIBXML_STATIC")
endif()
set(LIBXML2_FOUND TRUE)

set(PACKAGE_EXPORT
    "LIBXML2_INCLUDE_DIR;LIBXML2_INCLUDE_DIRS;LIBXML2_LIBRARIES;LIBXML2_FOUND;LIBXML2_DEFINITIONS"
)

#
