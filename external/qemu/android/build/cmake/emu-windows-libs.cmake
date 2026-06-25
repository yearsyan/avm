function(FIND_DIA_SDK)
  # --- Initial Configuration & Pre-requisites ---

  # Determine VSINSTALLDIR, preferring an existing CMake variable/cache, then environment variable.
  if(DEFINED VSINSTALLDIR)
    set(VSINSTALLDIR_LOCAL "${VSINSTALLDIR}")
  elseif(DEFINED ENV{VSINSTALLDIR})
    set(VSINSTALLDIR_LOCAL "$ENV{VSINSTALLDIR}")
    # If sourced from ENV, also set it as a CACHE variable for user visibility/modification
    set(VSINSTALLDIR "${VSINSTALLDIR_LOCAL}" CACHE PATH "Visual Studio Installation Directory (from ENV{VSINSTALLDIR})" FORCE)
  else()
    set(VSINSTALLDIR_LOCAL "")
  endif()

  if(NOT VSINSTALLDIR_LOCAL OR NOT IS_DIRECTORY "${VSINSTALLDIR_LOCAL}")
    message(WARNING "FIND_DIA_SDK: VSINSTALLDIR is not set or not a valid directory: '${VSINSTALLDIR_LOCAL}'. Cannot locate DIA SDK.")
    set(DIASDK_FOUND_LOCAL FALSE) # Use a local status variable
    # Propagate results to parent scope before early exit
    set(DIASDK_FOUND ${DIASDK_FOUND_LOCAL} PARENT_SCOPE)
    unset(DIASDK_INCLUDE_DIRS PARENT_SCOPE)
    unset(DIASDK_LIBRARIES PARENT_SCOPE)
    unset(DIASDK_MSDIA_DLL_COPY_DIR PARENT_SCOPE) # Ensure this is unset
    return()
  endif()

  # Define the target architecture for DIA SDK components.
  set(DIA_SDK_ARCH_SUBDIR_LOCAL "amd64") # Local variable
  message(STATUS "FIND_DIA_SDK: Targeting architecture: ${DIA_SDK_ARCH_SUBDIR_LOCAL}")
  message(STATUS "FIND_DIA_SDK: Using VSINSTALLDIR: [${VSINSTALLDIR_LOCAL}]")

  # --- Find DIA SDK Include Directory ---
  find_path(DIASDK_INCLUDE_DIR_LOCAL # Local variable
            NAMES dia2.h
            HINTS "${VSINSTALLDIR_LOCAL}/DIA SDK/include"
            DOC "Path to DIA SDK header files (dia2.h)")

  if(DIASDK_INCLUDE_DIR_LOCAL)
    message(STATUS "FIND_DIA_SDK: Found include directory: ${DIASDK_INCLUDE_DIR_LOCAL}")
  else()
    message(WARNING "FIND_DIA_SDK: Include directory (dia2.h) NOT FOUND. Searched in '${VSINSTALLDIR_LOCAL}/DIA SDK/include'.")
  endif()

  # --- Find DIA SDK Library (diaguids.lib) ---
  set(DIASDK_LIB_HINTS_LOCAL) # Local variable
  if(DIASDK_INCLUDE_DIR_LOCAL) # Prefer path relative to a found include directory
      list(APPEND DIASDK_LIB_HINTS_LOCAL "${DIASDK_INCLUDE_DIR_LOCAL}/../lib/${DIA_SDK_ARCH_SUBDIR_LOCAL}")
  endif()
  list(APPEND DIASDK_LIB_HINTS_LOCAL "${VSINSTALLDIR_LOCAL}/DIA SDK/lib/${DIA_SDK_ARCH_SUBDIR_LOCAL}") # Direct path
  if(DIASDK_LIB_HINTS_LOCAL)
    list(REMOVE_DUPLICATES DIASDK_LIB_HINTS_LOCAL)
  endif()

  find_library(DIASDK_GUIDS_LIBRARY_LOCAL # Local variable
               NAMES diaguids.lib
               HINTS ${DIASDK_LIB_HINTS_LOCAL}
               DOC "Path to DIA SDK library (diaguids.lib for ${DIA_SDK_ARCH_SUBDIR_LOCAL})")

  if(DIASDK_GUIDS_LIBRARY_LOCAL)
    message(STATUS "FIND_DIA_SDK: Found library (diaguids.lib): ${DIASDK_GUIDS_LIBRARY_LOCAL}")
  else()
    message(WARNING "FIND_DIA_SDK: Library (diaguids.lib for ${DIA_SDK_ARCH_SUBDIR_LOCAL}) NOT FOUND. Searched in: ${DIASDK_LIB_HINTS_LOCAL}")
  endif()

  # --- Find DIA SDK Runtime DLL (msdia140.dll) ---
  set(MSDIA_DLL_HINTS_LOCAL) # Local variable
  if(DIASDK_INCLUDE_DIR_LOCAL) # Prefer path relative to a found include directory
      list(APPEND MSDIA_DLL_HINTS_LOCAL "${DIASDK_INCLUDE_DIR_LOCAL}/../bin/${DIA_SDK_ARCH_SUBDIR_LOCAL}")
  endif()
  list(APPEND MSDIA_DLL_HINTS_LOCAL "${VSINSTALLDIR_LOCAL}/DIA SDK/bin/${DIA_SDK_ARCH_SUBDIR_LOCAL}") # Direct path
  if(MSDIA_DLL_HINTS_LOCAL)
    list(REMOVE_DUPLICATES MSDIA_DLL_HINTS_LOCAL)
  endif()

  find_file(MSDIA140_DLL_LOCATION_LOCAL # Local variable
            NAMES msdia140.dll
            HINTS ${MSDIA_DLL_HINTS_LOCAL}
            DOC "Path to DIA SDK runtime DLL (msdia140.dll for ${DIA_SDK_ARCH_SUBDIR_LOCAL})")

  # --- Setup msdia140.dll copy if found (at CMake configure time) ---
  set(DLL_DESTINATION_DIR_FOR_PROPAGATION "") # Variable to hold the path for parent scope

  if(MSDIA140_DLL_LOCATION_LOCAL AND EXISTS "${MSDIA140_DLL_LOCATION_LOCAL}")
    message(STATUS "FIND_DIA_SDK: Found runtime DLL (msdia140.dll): ${MSDIA140_DLL_LOCATION_LOCAL}")

    # Determine destination directory for the DLL copy
    set(DLL_DESTINATION_DIR_LOCAL "")
    if(CMAKE_RUNTIME_OUTPUT_DIRECTORY AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY STREQUAL "")
      set(DLL_DESTINATION_DIR_LOCAL "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
      message(STATUS "FIND_DIA_SDK: Will ensure msdia140.dll is in CMAKE_RUNTIME_OUTPUT_DIRECTORY: ${DLL_DESTINATION_DIR_LOCAL} (configure time).")
    else()
      # Fallback to a common 'bin' directory in the top-level build directory
      set(DLL_DESTINATION_DIR_LOCAL "${CMAKE_BINARY_DIR}/bin")
      message(STATUS "FIND_DIA_SDK: CMAKE_RUNTIME_OUTPUT_DIRECTORY is not set or is empty. Will ensure msdia140.dll is in default location: ${DLL_DESTINATION_DIR_LOCAL} (configure time).")
    endif()

    # Ensure the destination path is absolute
    if(NOT IS_ABSOLUTE "${DLL_DESTINATION_DIR_LOCAL}")
        get_filename_component(DLL_DESTINATION_DIR_LOCAL "${CMAKE_CURRENT_BINARY_DIR}/${DLL_DESTINATION_DIR_LOCAL}" ABSOLUTE)
    endif()
    message(STATUS "FIND_DIA_SDK: Absolute msdia140.dll copy destination set to: ${DLL_DESTINATION_DIR_LOCAL}")
    set(DLL_DESTINATION_DIR_FOR_PROPAGATION "${DLL_DESTINATION_DIR_LOCAL}") # Save for propagation

    get_filename_component(MSDIA140_DLL_NAME_LOCAL "${MSDIA140_DLL_LOCATION_LOCAL}" NAME)
    set(DEST_DLL_PATH_LOCAL "${DLL_DESTINATION_DIR_LOCAL}/${MSDIA140_DLL_NAME_LOCAL}")

    # Perform the copy at CMake configure time
    file(MAKE_DIRECTORY "${DLL_DESTINATION_DIR_LOCAL}") # Ensure directory exists

    execute_process(
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${MSDIA140_DLL_LOCATION_LOCAL}"
              "${DEST_DLL_PATH_LOCAL}"
      RESULT_VARIABLE copy_result
    )

    if(NOT copy_result EQUAL 0)
      message(WARNING "FIND_DIA_SDK: Failed to copy msdia140.dll to ${DEST_DLL_PATH_LOCAL} during CMake configuration. Result: ${copy_result}")
      set(DLL_DESTINATION_DIR_FOR_PROPAGATION "") # Do not propagate if copy failed
    else()
      message(STATUS "FIND_DIA_SDK: Ensured msdia140.dll is up-to-date in ${DEST_DLL_PATH_LOCAL} (configure time).")
    endif()
  else()
    message(WARNING "FIND_DIA_SDK: Runtime DLL (msdia140.dll for ${DIA_SDK_ARCH_SUBDIR_LOCAL}) NOT FOUND. Searched in: ${MSDIA_DLL_HINTS_LOCAL}. DLL will not be copied.")
  endif()

  # --- Prepare Link Libraries (only diaguids.lib now) ---
  set(PACKAGE_INTERFACE_LINK_LIBS_LOCAL "") # Local variable
  if(DIASDK_GUIDS_LIBRARY_LOCAL)
    list(APPEND PACKAGE_INTERFACE_LINK_LIBS_LOCAL "${DIASDK_GUIDS_LIBRARY_LOCAL}")
  endif()

  # --- Create and Configure diaguids::diaguids INTERFACE Target ---
  set(DIASDK_FOUND_LOCAL FALSE) # Initialize local found status

  if(DIASDK_INCLUDE_DIR_LOCAL AND DIASDK_GUIDS_LIBRARY_LOCAL)
    if(NOT TARGET diaguids::diaguids)
      add_library(diaguids::diaguids INTERFACE IMPORTED) # INTERFACE IMPORTED targets are globally visible by name
      message(STATUS "FIND_DIA_SDK: Created INTERFACE IMPORTED target diaguids::diaguids.")
    endif()

    set_target_properties(diaguids::diaguids PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${DIASDK_INCLUDE_DIR_LOCAL}"
      INTERFACE_LINK_LIBRARIES "${PACKAGE_INTERFACE_LINK_LIBS_LOCAL}" # Will only contain diaguids.lib
    )
    message(STATUS "FIND_DIA_SDK: Configured diaguids::diaguids. INTERFACE_LINK_LIBRARIES: ${PACKAGE_INTERFACE_LINK_LIBS_LOCAL}")
    set(DIASDK_FOUND_LOCAL TRUE)
  else()
    message(WARNING "FIND_DIA_SDK: Essential components (include directory or diaguids.lib) NOT FOUND. diaguids::diaguids target will not be fully configured.")
    if(TARGET diaguids::diaguids) # If target somehow exists, clear its properties
        set_target_properties(diaguids::diaguids PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ""
            INTERFACE_LINK_LIBRARIES ""
        )
    endif()
  endif()

  # --- Propagate results to parent scope ---
  set(DIASDK_FOUND ${DIASDK_FOUND_LOCAL} PARENT_SCOPE)
  if(DIASDK_FOUND_LOCAL)
    set(DIASDK_INCLUDE_DIRS ${DIASDK_INCLUDE_DIR_LOCAL} PARENT_SCOPE)
    set(DIASDK_LIBRARIES "${PACKAGE_INTERFACE_LINK_LIBS_LOCAL}" PARENT_SCOPE) # Only diaguids.lib
    # Propagate the DLL copy directory if the copy was attempted and successful (or path determined)
    if(DLL_DESTINATION_DIR_FOR_PROPAGATION AND NOT DLL_DESTINATION_DIR_FOR_PROPAGATION STREQUAL "")
      set(DIASDK_MSDIA_DLL_COPY_DIR "${DLL_DESTINATION_DIR_FOR_PROPAGATION}" PARENT_SCOPE)
    else()
      unset(DIASDK_MSDIA_DLL_COPY_DIR PARENT_SCOPE)
    endif()
  else()
    unset(DIASDK_INCLUDE_DIRS PARENT_SCOPE)
    unset(DIASDK_LIBRARIES PARENT_SCOPE)
    unset(DIASDK_MSDIA_DLL_COPY_DIR PARENT_SCOPE)
  endif()

endfunction()

# This function discovers the standard windows libraries.
# Linking in these libraries is done differently in MSVC vs Clang/Mingw
#
# This function will declare the library ${NAME}::${NAME} that you can
# take a dependency on, turning it into to the correct dependency.
function(android_find_windows_library NAME)
  if(MSVC)
    find_library(${NAME}_LIBRARIES ${NAME})
  else()
    set(${NAME}_LIBRARIES "${NAME}")
  endif()

  if(NOT TARGET ${NAME}::${NAME})
    add_library(${NAME}::${NAME} INTERFACE IMPORTED GLOBAL)
    set_target_properties(${NAME}::${NAME} PROPERTIES INTERFACE_LINK_LIBRARIES ${${NAME}_LIBRARIES})
    android_license(TARGET "${NAME}::${NAME}" LIBNAME None SPDX None LICENSE None LOCAL None)
  endif()
endfunction()

set(WINDOWS_LIBS
    advapi32
    amstrmid
    atls
    cfgmgr32
    comsuppw
    crypt32
    d3d9
    delayimp
    dbghelp
    diaguids
    dismapi
    dmoguids
    dxguid
    gdi32
    imagehlp
    iphlpapi
    mfuuid
    mincore
    msdmo
    normaliz
    ole32
    oleaut32
    powrprof
    propsys
    psapi
    secur32
    setupapi
    shell32
    shlwapi
    strmiids
    user32
    version
    wbemuuid
    wininet
    winhttp
    winmm
    wldap32
    wmcodecdspuuid
    ws2_32
    )
foreach(LIB ${WINDOWS_LIBS})
  android_find_windows_library(${LIB})
endforeach()

if(WIN32 AND MSVC)
   # We expect all the visual studio variables to be set. The rebuild script will take care of this..
   # If you are building from
  get_filename_component(VCTOOLS_PATH "$ENV{VCTOOLSINSTALLDIR}" ABSOLUTE CACHE)
  get_filename_component(VSINSTALLDIR "$ENV{VSINSTALLDIR}" ABSOLUTE CACHE)
  get_filename_component(WINSDKDIR "$ENV{WindowsSdkDir}" ABSOLUTE CACHE)

  FIND_DIA_SDK()

  if(DIASDK_FOUND)
    message(STATUS "DIA SDK was found successfully by FIND_DIA_SDK function.")
    message(STATUS "  Include Dirs: ${DIASDK_INCLUDE_DIRS}")
    message(STATUS "  Libraries   : ${DIASDK_LIBRARIES}") # Note: This no longer includes the DLL runtime target
  else()
    message(FATAL_ERROR "DIA SDK was NOT found by FIND_DIA_SDK function.")
  endif()


  # Find the ATL path, it will typically look something like this.
  #C:\Program Files (x86)\Microsoft Visual Studio\2017\xxx\VC\Tools\MSVC\14.16.27023\atlmfc\include
  message(STATUS "Looking for atls: [${VCTOOLS_PATH}]")
  find_path(ATL_INCLUDE_DIR
            atlbase.h # Find a path with atlbase.h
            HINTS "${VCTOOLS_PATH}/atlmfc/include"
            DOC "path to ATL header files")
  message(STATUS "Found ATL Include: ${ATL_INCLUDE_DIR}")

  find_library(ATL_LIBRARY NAMES atls.lib HINTS ${ATL_INCLUDE_DIR}/../lib/x64)
  set_target_properties(atls::atls
                        PROPERTIES INTERFACE_LINK_LIBRARIES ${ATL_LIBRARY} INTERFACE_INCLUDE_DIRECTORIES
                                   ${ATL_INCLUDE_DIR})

  # Find the dismapi path, it will typically look something like this.
  #C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Deployment Tools\SDKs\DismApi
  message(STATUS "Looking for diamapi: [${WINSDKDIR}]")
  find_path(DISMAPI_INCLUDE_DIR
            dismapi.h # Find a path with dismapi.h
	    HINTS "${WINSDKDIR}/Assessment and Deployment Kit/Deployment Tools/SDKs/DismApi/Include"
	    DOC "path to dism api header files")
  message(STATUS "Found dismapi Include: ${DISMAPI_INCLUDE_DIR}")

  find_library(DISMAPI_LIBRARY NAMES dismapi.lib HINTS ${DISMAPI_INCLUDE_DIR}/../lib/amd64)
  message(STATUS "Found dismapi Library: ${DISMAPI_LIBRARY}")
  set_target_properties(dismapi::dismapi
                        PROPERTIES INTERFACE_LINK_LIBRARIES ${DISMAPI_LIBRARY} INTERFACE_INCLUDE_DIRECTORIES
                                   ${DISMAPI_INCLUDE_DIR})

endif()
