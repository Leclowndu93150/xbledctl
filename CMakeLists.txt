cmake_minimum_required(VERSION 3.20)
project(xbledctl VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_win32.cpp
    ${IMGUI_DIR}/imgui_impl_dx11.cpp
)

add_executable(xbledctl WIN32
    src/main.cpp
    src/xbox_led.c
    ${IMGUI_SOURCES}
    res/app.rc
)

target_include_directories(xbledctl PRIVATE
    ${CMAKE_SOURCE_DIR}/imgui
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(xbledctl PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dwmapi
)

set_target_properties(xbledctl PROPERTIES
    LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_SOURCE_DIR}/res/app.manifest"
)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")

set_target_properties(xbledctl PROPERTIES
    WIN32_EXECUTABLE TRUE
)
