find_library(STARLET_EGL_LIBRARY EGL REQUIRED)
find_package(ZLIB REQUIRED)
add_executable(starlet-replay replay.cpp png.cpp shaders.cpp)
if(NOT STARLET_GX_TRACE)
    # A recording build has it in starlet_gx already.
    target_sources(starlet-replay PRIVATE "${PROJECT_SOURCE_DIR}/src/gx/trace/access.cpp")
endif()
target_include_directories(starlet-replay PRIVATE "${PROJECT_SOURCE_DIR}/src/gx" "${STARLET_GLES_INCLUDE_DIR}")
target_link_libraries(starlet-replay PRIVATE starlet_gx_gles "${STARLET_EGL_LIBRARY}" ZLIB::ZLIB)
