cmake_minimum_required(VERSION 3.20)
project(repkg VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REPKG_READER_SOURCES
    src/middleware/content/packages/reader/package_block_cache.cpp
    src/middleware/content/packages/reader/package_class_scan.cpp
    src/middleware/content/packages/reader/package_entry_reader.cpp
    src/middleware/content/packages/reader/package_file_locator.cpp
    src/middleware/content/packages/reader/package_handle_cache.cpp
    src/middleware/content/packages/reader/package_header.cpp
    src/middleware/content/packages/reader/package_locator_cache.cpp
    src/middleware/content/packages/reader/package_parallel_read.cpp
    src/middleware/content/packages/reader/package_table_cache.cpp
)

set(REPKG_PACKAGE_SOURCES
    src/package.cpp
    src/texture.cpp
    src/ui.cpp
    src/middleware/compression/oodle/oodle_installed.cpp
    src/middleware/compression/oodle/oodle_runtime.cpp
    src/middleware/crypto/aes_gcm_decrypt.cpp
    ${REPKG_READER_SOURCES}
)

add_executable(repkg
    src/main.cpp
    src/json.cpp
    src/localized.cpp
    ${REPKG_PACKAGE_SOURCES}
)

add_executable(depkg
    src/depkg.cpp
    src/json.cpp
    ${REPKG_PACKAGE_SOURCES}
)

foreach(REPKG_TARGET IN ITEMS repkg depkg)
    target_include_directories(${REPKG_TARGET} PRIVATE src)
    target_compile_definitions(${REPKG_TARGET} PRIVATE
        WIN32
        _WINDOWS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    if(MSVC)
        target_compile_options(${REPKG_TARGET} PRIVATE /utf-8 /W4 /EHsc /MT)
    endif()
    target_link_libraries(${REPKG_TARGET} PRIVATE kernel32 bcrypt)

    add_custom_command(TARGET ${REPKG_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
                "$<TARGET_FILE_DIR:${REPKG_TARGET}>/README.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                "$<TARGET_FILE_DIR:${REPKG_TARGET}>/LICENSE"
    )
endforeach()

add_custom_target(repkg_ui_editor ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/editor"
            "$<TARGET_FILE_DIR:repkg>/ui-editor"
    DEPENDS
        editor/index.html
        editor/app.css
        editor/app.js
        editor/open-ui-editor.cmd
        editor/open-ui-editor.sh
)
