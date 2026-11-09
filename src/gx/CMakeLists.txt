add_library(starlet_gx STATIC
    state.cpp
    decoder.cpp
    vertex.cpp
    vertex_stream.cpp
    line_point.cpp
    transform.cpp
    tev.cpp
    texture.cpp
    sampling.cpp
    indirect.cpp
    raster.cpp
    pixel.cpp
    efb_format.cpp
    depth.cpp
    efb_clear.cpp
    display_copy.cpp
    texture_copy.cpp
    draw.cpp
    direct.cpp
    fast_shader.cpp
)
add_library(starlet::gx ALIAS starlet_gx)
target_include_directories(starlet_gx PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_gx PUBLIC cxx_std_17)
set_target_properties(starlet_gx PROPERTIES CXX_EXTENSIONS OFF)

# The renderer compiles wherever OpenGL ES 3.1 headers exist. Linking the GL library is
# left to STARLET_GLES, so host tests can supply their own GL entry points instead.
find_path(STARLET_GLES_INCLUDE_DIR GLES3/gl31.h)
if(STARLET_GLES AND NOT STARLET_GLES_INCLUDE_DIR)
    message(FATAL_ERROR "STARLET_GLES requires OpenGL ES 3.1 headers (set STARLET_GLES_INCLUDE_DIR)")
endif()
if(STARLET_GLES_INCLUDE_DIR)
    find_package(Threads REQUIRED)
    add_library(starlet_gx_gles_objects OBJECT
        gles/renderer.cpp
        gles/direct.cpp
        gles/program_builder.cpp
        gles/program_cache.cpp
        gles/program_list.cpp
    )
    target_include_directories(starlet_gx_gles_objects PRIVATE "${STARLET_GLES_INCLUDE_DIR}")
    target_link_libraries(starlet_gx_gles_objects PUBLIC starlet_gx Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(STARLET_GLES)
    find_library(STARLET_GLES_LIBRARY GLESv2 REQUIRED)
    add_library(starlet_gx_gles STATIC)
    add_library(starlet::gx_gles ALIAS starlet_gx_gles)
    target_link_libraries(starlet_gx_gles PUBLIC starlet_gx_gles_objects "${STARLET_GLES_LIBRARY}")
endif()
