cmake_minimum_required(VERSION 3.20)
project(xbledctl VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# --- ImGui sources ---
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_win32.cpp
    ${IMGUI_DIR}/imgui_impl_dx11.cpp
)

# --- Executable ---
add_executable(xbledctl WIN32
    src/main.cpp
    src/xbox_led.c
    ${IMGUI_SOURCES}
    res/app.rc
)

target_include_directories(xbledctl PRIVATE
    ${CMAKE_SOURCE_DIR}/imgui
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/src
)

# --- libusb (dynamic link via import lib) ---
target_link_libraries(xbledctl PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/libusb-1.0.lib
)

# --- DirectX 11 + Win32 ---
target_link_libraries(xbledctl PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dwmapi
)

# --- Embed DPI-aware manifest via linker ---
set_target_properties(xbledctl PROPERTIES
    LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_SOURCE_DIR}/res/app.manifest"
)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")

# --- Copy libusb DLL next to exe ---
add_custom_command(TARGET xbledctl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/lib/libusb-1.0.dll
        $<TARGET_FILE_DIR:xbledctl>/libusb-1.0.dll
)

# No console window
set_target_properties(xbledctl PROPERTIES
    WIN32_EXECUTABLE TRUE
)
