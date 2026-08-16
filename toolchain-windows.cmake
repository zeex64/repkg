cmake_minimum_required(VERSION 3.20)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)

if(NOT DEFINED XWIN_DIR)
    set(XWIN_DIR "${CMAKE_CURRENT_LIST_DIR}/.xwin-cache")
endif()
get_filename_component(XWIN_DIR "${XWIN_DIR}" ABSOLUTE)

set(REPKG_COMPILE_FLAGS
    "-target x86_64-pc-windows-msvc /winsdkdir \"${XWIN_DIR}/sdk\" /vctoolsdir \"${XWIN_DIR}/crt\"")
set(CMAKE_C_FLAGS "${REPKG_COMPILE_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${REPKG_COMPILE_FLAGS}" CACHE STRING "" FORCE)

set(REPKG_LINK_FLAGS
    "/libpath:\"${XWIN_DIR}/crt/lib/x86_64\" /libpath:\"${XWIN_DIR}/sdk/lib/um/x86_64\" /libpath:\"${XWIN_DIR}/sdk/lib/ucrt/x86_64\"")
set(CMAKE_EXE_LINKER_FLAGS "${REPKG_LINK_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
