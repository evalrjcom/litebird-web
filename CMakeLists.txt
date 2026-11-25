cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
project(litebird LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(CURL REQUIRED)
find_package(FLTK CONFIG REQUIRED)

add_executable(litebird litebird.cpp)
target_link_libraries(litebird PRIVATE CURL::libcurl fltk fltk_forms fltk_images fltk_gl)

if(WIN32)
  # curl headers need this when the library is static.
  target_compile_definitions(litebird PRIVATE CURL_STATICLIB)
  # Schannel / WinHTTP-adjacent system libs used by static libcurl on Windows.
  target_link_libraries(litebird PRIVATE
    ws2_32
    crypt32
    normaliz
    wldap32
    bcrypt
    advapi32
  )
endif()
