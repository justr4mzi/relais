cmake_minimum_required(VERSION 3.20)
project(Relais VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(relais WIN32
    src/main.cpp
    src/video.cpp
    src/audio.cpp
    src/render.cpp
    res/relais.rc
)

target_compile_definitions(relais PRIVATE UNICODE _UNICODE)
if (MSVC)
    target_compile_options(relais PRIVATE /utf-8 /W3)
    # The manifest is embedded by relais.rc
    target_link_options(relais PRIVATE /MANIFEST:NO /ENTRY:wWinMainCRTStartup)
    set_property(TARGET relais PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_options(relais PRIVATE -municode)
endif()

target_link_libraries(relais PRIVATE d3d11 dxgi mfplat mf mfreadwrite mfuuid ole32 uuid comctl32 gdi32 user32 shell32)
