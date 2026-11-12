# The AX voice library for SDK-level ports: voice allocation and sync as the SDK does it, and the
# DSP microcode's mixing, played through starlet::ai. Linux, C++17.
if(NOT TARGET starlet::ai)
    return()
endif()
add_library(starlet_ax STATIC
    ax_sdk.cpp
    dsp.cpp
    platform_starlet.cpp
)
add_library(starlet::ax ALIAS starlet_ax)
target_include_directories(starlet_ax PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_ax PUBLIC cxx_std_17)
set_target_properties(starlet_ax PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(starlet_ax PUBLIC starlet::ai starlet::ar starlet::os)
