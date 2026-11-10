# Test images are compressed with the pinned zstd's own compressor.
file(GLOB starlet_zstd_compress_sources "${PROJECT_SOURCE_DIR}/third_party/zstd/lib/compress/*.c")
add_library(starlet_zstd_compress STATIC ${starlet_zstd_compress_sources})
target_compile_definitions(starlet_zstd_compress PRIVATE ZSTD_DISABLE_ASM=1)
set_target_properties(starlet_zstd_compress PROPERTIES C_STANDARD 99)
target_link_libraries(starlet_zstd_compress PUBLIC starlet_zstd)

find_package(Threads REQUIRED)
function(starlet_disc_test name)
    add_executable(starlet_disc_${name}_tests ${name}_tests.cpp)
    target_link_libraries(starlet_disc_${name}_tests PRIVATE starlet_disc starlet_zstd_compress Threads::Threads)
    add_test(NAME starlet_disc_${name} COMMAND starlet_disc_${name}_tests)
endfunction()

foreach(name image fst install)
    starlet_disc_test(${name})
endforeach()
