cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
project(litebird LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# CHANGED: default to an optimised build for single-config generators (make/ninja).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(CURL REQUIRED)
find_package(FLTK CONFIG REQUIRED)
# FIX: worker-thread page loading needs the platform thread library.
find_package(Threads REQUIRED)

# CHANGED: WIN32 builds a GUI-subsystem exe, so no console window opens next
# to the browser (FLTK supplies the WinMain -> main() shim).
add_executable(litebird WIN32 litebird.cpp)

# CHANGED: link only the core FLTK library. fltk_forms, fltk_images and
# fltk_gl were unused; fltk_gl also dragged in OPENGL32.dll.
# FIX: prefer the namespaced target exported by FLTK 1.4 (vcpkg), fall back
# to the plain target name exported by FLTK 1.3 (Debian/Ubuntu).
if(TARGET fltk::fltk)
  set(LITEBIRD_FLTK fltk::fltk)
else()
  set(LITEBIRD_FLTK fltk)
endif()
target_link_libraries(litebird PRIVATE CURL::libcurl ${LITEBIRD_FLTK} Threads::Threads)

# CHANGED: warnings on, so regressions show up in CI logs.
if(MSVC)
  target_compile_options(litebird PRIVATE /W4 /utf-8)
else()
  target_compile_options(litebird PRIVATE -Wall -Wextra)
endif()

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

# CHANGED: `cmake --install` support.
install(TARGETS litebird RUNTIME DESTINATION bin)
