# RVZ groups are Zstandard frames. The pinned decoder is linked statically, since some
# handheld firmware ships no libzstd.
set(STARLET_ZSTD_DIR "${PROJECT_SOURCE_DIR}/third_party/zstd/lib")
if(NOT EXISTS "${STARLET_ZSTD_DIR}/zstd.h")
    message(FATAL_ERROR "third_party/zstd is missing; run git submodule update --init third_party/zstd")
endif()
file(GLOB starlet_zstd_sources "${STARLET_ZSTD_DIR}/common/*.c" "${STARLET_ZSTD_DIR}/decompress/*.c")
add_library(starlet_zstd STATIC ${starlet_zstd_sources})
target_include_directories(starlet_zstd SYSTEM PUBLIC "${STARLET_ZSTD_DIR}")
target_compile_definitions(starlet_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0)
set_target_properties(starlet_zstd PROPERTIES C_STANDARD 99)

find_package(Threads REQUIRED)
add_library(starlet_disc STATIC
    image.cpp
    fst.cpp
    sha1.cpp
    install.cpp
)
add_library(starlet::disc ALIAS starlet_disc)
target_include_directories(starlet_disc PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_disc PUBLIC cxx_std_17)
target_compile_definitions(starlet_disc PRIVATE _FILE_OFFSET_BITS=64)
set_target_properties(starlet_disc PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(starlet_disc PRIVATE starlet_zstd Threads::Threads)
