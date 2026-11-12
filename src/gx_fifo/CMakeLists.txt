# The GX FIFO and VI SDK calls for SDK-level ports: the write-gather pipe, FIFO objects, GX
# synchronization and the GPU thread running Starlet's decoder, plus the retrace interrupt. Linux, C++17.
find_package(Threads REQUIRED)
add_library(starlet_gx_fifo STATIC
    frame_trace.cpp
    gles_renderer.cpp
    gpu.cpp
    gx_sdk.cpp
    video.cpp
)
add_library(starlet::gx_fifo ALIAS starlet_gx_fifo)
target_include_directories(starlet_gx_fifo PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_gx_fifo PUBLIC cxx_std_17)
set_target_properties(starlet_gx_fifo PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(starlet_gx_fifo PUBLIC starlet::os starlet::gx Threads::Threads)
# GlesBackend (gles_renderer.cpp) needs only the renderer's header to compile; ports that use it link
# the GL library through starlet::gx_gles.
if(TARGET starlet::gx_gles)
    target_link_libraries(starlet_gx_fifo PUBLIC starlet::gx_gles)
endif()
