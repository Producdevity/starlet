function(starlet_gx_test name)
    add_executable(starlet_gx_${name}_tests ${name}_tests.cpp)
    target_link_libraries(starlet_gx_${name}_tests PRIVATE starlet_gx)
    add_test(NAME starlet_gx_${name} COMMAND starlet_gx_${name}_tests)
endfunction()

foreach(name core vertex vertex_stream line_point transform tev texture texture_copy_resident raster pixel
             efb_format depth draw z_texture)
    starlet_gx_test(${name})
endforeach()
set_tests_properties(starlet_gx_pixel starlet_gx_efb_format PROPERTIES TIMEOUT 60)

# The TEV test writes shader fixtures; libstdc++ before GCC 9 keeps <filesystem> in stdc++fs.
include(CheckCXXSourceCompiles)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(filesystem_probe [[#include <filesystem>
int main() { return std::filesystem::create_directories("probe"); }]])
check_cxx_source_compiles("${filesystem_probe}" STARLET_FILESYSTEM_BUILTIN)
if(NOT STARLET_FILESYSTEM_BUILTIN)
    set(CMAKE_REQUIRED_LIBRARIES stdc++fs)
    check_cxx_source_compiles("${filesystem_probe}" STARLET_FILESYSTEM_SEPARATE)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(NOT STARLET_FILESYSTEM_SEPARATE)
        message(FATAL_ERROR "The TEV fixture writer requires C++17 filesystem support")
    endif()
    target_link_libraries(starlet_gx_tev_tests PRIVATE stdc++fs)
endif()

# Renderer tests run against a recording GL fake instead of a driver.
if(TARGET starlet_gx_gles_objects)
    add_executable(starlet_gx_gles_renderer_tests gles/renderer_tests.cpp gles/fake_gles.cpp)
    target_include_directories(starlet_gx_gles_renderer_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${STARLET_GLES_INCLUDE_DIR}")
    target_link_libraries(starlet_gx_gles_renderer_tests PRIVATE starlet_gx_gles_objects)
    add_test(NAME starlet_gx_gles_renderer COMMAND starlet_gx_gles_renderer_tests)

    add_executable(starlet_gx_gles_program_cache_tests gles/program_cache_tests.cpp gles/fake_gles.cpp)
    target_include_directories(starlet_gx_gles_program_cache_tests PRIVATE "${STARLET_GLES_INCLUDE_DIR}")
    target_link_libraries(starlet_gx_gles_program_cache_tests PRIVATE starlet_gx_gles_objects)
    add_test(NAME starlet_gx_gles_program_cache COMMAND starlet_gx_gles_program_cache_tests)
endif()
