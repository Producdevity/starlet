# The firmware's SDL2: the target sysroot's in a cross build, Homebrew's sdl2-compat on a Mac.
find_path(STARLET_SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
find_library(STARLET_SDL2_LIBRARY NAMES SDL2 SDL2-2.0)
if(STARLET_SDL2_INCLUDE_DIR AND STARLET_SDL2_LIBRARY)
    set(starlet_sdl2_found ON)
else()
    set(starlet_sdl2_found OFF)
endif()
option(STARLET_HOST "Build starlet::host: SDL2 audio output, controllers and quit handling" ${starlet_sdl2_found})
if(NOT STARLET_HOST)
    return()
endif()
if(NOT starlet_sdl2_found)
    message(FATAL_ERROR "STARLET_HOST requires SDL2 (set STARLET_SDL2_INCLUDE_DIR and STARLET_SDL2_LIBRARY)")
endif()

find_package(Threads REQUIRED)
add_library(starlet_host STATIC
    audio_output.cpp
    controllers.cpp
    pad.cpp
    quit.cpp
)
add_library(starlet::host ALIAS starlet_host)
target_include_directories(starlet_host PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_include_directories(starlet_host SYSTEM PRIVATE "${STARLET_SDL2_INCLUDE_DIR}")
target_compile_features(starlet_host PUBLIC cxx_std_17)
set_target_properties(starlet_host PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(starlet_host PUBLIC "${STARLET_SDL2_LIBRARY}" Threads::Threads)
