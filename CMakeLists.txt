cmake_minimum_required(VERSION 3.16)
project(SlideCut CXX)
include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI third_party/imgui)

add_executable(SlideCut WIN32
    src/main.cpp
    third_party/impl.cpp
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp
    ${IMGUI}/backends/imgui_impl_dx11.cpp
)

target_include_directories(SlideCut PRIVATE ${IMGUI} ${IMGUI}/backends third_party)
target_compile_definitions(SlideCut PRIVATE UNICODE _UNICODE)
target_link_libraries(SlideCut PRIVATE d3d11 dxgi d3dcompiler ole32 uuid dwmapi winmm
                                       shell32 shlwapi dbghelp)
if(BUILD_TESTING)
    add_test(NAME workspace_compatibility COMMAND SlideCut --workspace-test)
    add_test(NAME workspace_interface COMMAND SlideCut --workspace-ui)
    find_program(SLIDECUT_FFMPEG ffmpeg)
    if(SLIDECUT_FFMPEG)
        add_test(NAME workspace_media COMMAND SlideCut --workspace-media)
        set_tests_properties(workspace_media PROPERTIES TIMEOUT 45)
    endif()
    # The projector HLSL is compiled at run time, so a clean build says nothing about
    # whether it works. Compile it here instead of finding out in the preview.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME shader_compilation COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_shaders.py)
    endif()
endif()

add_custom_command(TARGET SlideCut POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:SlideCut>/assets
    # The exe looks for its helper scripts beside itself first, so a copy left there
    # would shadow the real ones. Refresh them on every build instead.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/projector_render.py
            ${CMAKE_CURRENT_SOURCE_DIR}/clip_shader_export.py
            ${CMAKE_CURRENT_SOURCE_DIR}/surveillance_shader.h
            $<TARGET_FILE_DIR:SlideCut>)

if(NOT MSVC)
    # -g keeps the symbol tables in the exe. They cost disk, not speed, and without
    # them a crash dump is a list of raw addresses nobody can read.
    target_compile_options(SlideCut PRIVATE -O2 -g -municode)
    target_link_options(SlideCut PRIVATE -municode -static)
endif()
