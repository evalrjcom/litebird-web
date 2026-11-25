cmake_minimum_required(VERSION 3.16)
project(litebird LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(FLTK CONFIG REQUIRED)

add_executable(litebird litebird.cpp)
target_link_libraries(litebird PRIVATE CURL::libcurl fltk fltk_forms fltk_images fltk_gl)
