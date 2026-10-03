cmake_minimum_required(VERSION 3.15)
project(MonkeyTool VERSION 1.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/O2 /W3 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    add_compile_options(-O2 -Wall)
endif()

# the LZHAM decompressor (public domain), compiled in
file(GLOB LZHAM_SOURCES ${CMAKE_SOURCE_DIR}/src/lzham/*.cpp)
if(NOT MSVC)
    set_source_files_properties(${LZHAM_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

set(CORE_SOURCES
    src/nfsnl_core.cpp
    src/nfsnl_image.cpp
    src/nfsnl_model.cpp
    src/nfsnl_m3g.cpp
    src/nfsnl_render.cpp
    src/nfsnl_audio.cpp
    src/nfsnl_text.cpp
    src/nfsnl_library.cpp
    src/nfsnl_profiles.cpp
    src/nfsnl_rr3.cpp
    src/nfsnl_import.cpp
    src/nfsnl_data.cpp
    src/nfsnl_save.cpp
    src/nfsnl_sbin.cpp
    src/nfsnl_decode.cpp
    src/nfsnl_encode.cpp
    src/nfsnl_im2.cpp
    src/nfsnl_scene.cpp
    src/nfsnl_astc.cpp
    src/nfsnl_brotli.cpp
    src/nfsnl_jsr.cpp
    src/nfsnl_lzham.cpp
    ${LZHAM_SOURCES}
)

# ---- command line tool (all platforms) ----
add_executable(monkeytool_cli ${CORE_SOURCES} src/main_cli.cpp)
target_include_directories(monkeytool_cli PRIVATE src)
if(UNIX)
    target_link_libraries(monkeytool_cli dl)
endif()
if(WIN32)
    # the system's image codecs (WIC) for pictures being imported
    target_link_libraries(monkeytool_cli ole32)
endif()

# ---- Windows GUI ----
if(WIN32)
    # icon, version info and the visual-styles manifest
    set(MT_RESOURCES src/monkeytool.rc)

    add_executable(MonkeyTool WIN32 ${CORE_SOURCES} src/main_win32.cpp src/win32_save_editor.cpp ${MT_RESOURCES})
    target_include_directories(MonkeyTool PRIVATE src)
    target_link_libraries(MonkeyTool comctl32 comdlg32 shell32 ole32 uxtheme winmm opengl32 gdi32)
    set_target_properties(MonkeyTool PROPERTIES OUTPUT_NAME "MonkeyTool")

    # the .rc refers to monkeytool.ico / .manifest relative to itself
    set_source_files_properties(${MT_RESOURCES} PROPERTIES
        COMPILE_FLAGS "-I${CMAKE_SOURCE_DIR}/src")

    if(MINGW)
        # ship without needing the MinGW runtime DLLs next to the exe
        target_link_options(MonkeyTool PRIVATE
            -static -static-libgcc -static-libstdc++ -municode)
        target_link_options(monkeytool_cli PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()
