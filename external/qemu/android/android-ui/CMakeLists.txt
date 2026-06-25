# AEMU core-only / headless: only the five UI modules needed by the headless
# emulator core are built. The Qt UI shell (aemu-ui-qt, aemu-ui-widgets,
# aemu-ext-pages, aemu-recording, apps) has been removed.
#
# The helper below historically created "Qt" library targets, but it does not
# link Qt itself; it is a thin wrapper over android_add_library that the
# headless modules still rely on.

# ~~~
# ! android_qt_lib: Create a target with the given sources and dependencies
#   it will automatically export the _autogen include directory for all the files
#   for which uic and moc are run.
#
# ``TARGET``    The name of the library
# ``DLLEXPORT``  Define used to mark export/import
# ``SHARED``     Present if this should be a shared library
# ``SRC``        The set of sources
# ``ALIAS``      Alias under which this lib is known
# ``DEPS``       List of private dependencies needed to build this library.
# ~~
function(android_qt_lib)
  # Parse arguments
  set(options SHARED)
  set(oneValueArgs TARGET DLLEXPORT DLLENABLE ALIAS)
  set(multiValueArgs SRC DEPS INCLUDES)
  cmake_parse_arguments(ext "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  if(${ext_SHARED})
    message(FATAL_ERROR "${ext_TARGET} is shared, currently not supported.")
    android_add_library(TARGET ${ext_TARGET} SHARED LICENSE Apache-2.0
                        SOURCE_DIR src SRC ${ext_SRC})
    if(WINDOWS_MSVC_X86_64)
      target_compile_definitions(${ext_TARGET} PUBLIC ${ext_DLLENABLE}
                                 PRIVATE ${ext_DLLEXPORT})
    endif()
  else()
    android_add_library(TARGET ${ext_TARGET} LICENSE Apache-2.0 SOURCE_DIR src
                        SRC ${ext_SRC})
  endif()

  if(ext_INCLUDES)
    target_include_directories(${ext_TARGET} PUBLIC ${ext_INCLUDES})
  endif()
  if(NOT "${ext_ALIAS}" STREQUAL "")
    set_property(GLOBAL APPEND PROPERTY ALIAS_LST
                                        "${ext_ALIAS}|${ext_TARGET}\n")
    add_library(${ext_ALIAS} ALIAS ${ext_TARGET})
  endif()
  target_link_libraries(${ext_TARGET} PRIVATE ${ext_DEPS})
  target_include_directories(
    ${ext_TARGET}
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/${ext_TARGET}_autogen/include include
    PRIVATE src)
  # When using agents we use c-structs which have 0 size in c, but 1 size in
  # c++.
  target_compile_options(${ext_TARGET} PRIVATE -Wno-extern-c-compat)

  if(NOT BUILDING_FOR_AARCH64)
    target_compile_definitions(${ext_TARGET} PRIVATE USE_MMX=1)
    target_compile_options(${ext_TARGET} PRIVATE "-mmmx")
  endif()
endfunction()

add_subdirectory(modules/aemu-gl-bridge)
add_subdirectory(modules/aemu-gl-init)
add_subdirectory(modules/aemu-ui-headless)
add_subdirectory(modules/aemu-ui-common)
add_subdirectory(modules/aemu-ui-window)
