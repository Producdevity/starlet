cmake_minimum_required(VERSION 3.21)
project(Starlet LANGUAGES C CXX)

option(STARLET_GLES "Link the GX renderer against the target's OpenGL ES 3.1 library" OFF)
option(STARLET_TESTS "Build Starlet's tests" ${PROJECT_IS_TOP_LEVEL})

if(STARLET_TESTS)
    enable_testing()
endif()

add_subdirectory(src/gx)
add_subdirectory(src/disc)
if(STARLET_TESTS)
    add_subdirectory(tests/gx)
    add_subdirectory(tests/disc)
endif()
