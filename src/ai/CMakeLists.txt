# The audio interface DMA for SDK-level ports, played through starlet::host's AudioOutput. Linux, C++17.
if(NOT TARGET starlet::host)
    return()
endif()
add_library(starlet_ai STATIC
    ai.cpp
    platform_starlet.cpp
)
add_library(starlet::ai ALIAS starlet_ai)
target_include_directories(starlet_ai PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_ai PUBLIC cxx_std_17)
set_target_properties(starlet_ai PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(starlet_ai PUBLIC starlet::os starlet::host)
