cmake_minimum_required(VERSION 3.20)
project(PrintScreenRegionSnip LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(PrintScreenRegionSnip WIN32
    src/main.cpp
)

target_compile_definitions(PrintScreenRegionSnip PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(PrintScreenRegionSnip PRIVATE
    d3d11
    dxgi
    user32
    gdi32
)
