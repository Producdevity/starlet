add_executable(starlet_aurora_texture_convert_tests texture_convert_tests.cpp
               "${PROJECT_SOURCE_DIR}/integrations/aurora/starlet_backend/texture_convert.cpp")
target_include_directories(starlet_aurora_texture_convert_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/integrations/aurora/starlet_backend")
target_link_libraries(starlet_aurora_texture_convert_tests PRIVATE starlet_gx)
add_test(NAME starlet_aurora_texture_convert COMMAND starlet_aurora_texture_convert_tests)

add_executable(starlet_aurora_texture_writes_tests texture_writes_tests.cpp)
target_include_directories(starlet_aurora_texture_writes_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/integrations/aurora/starlet_backend")
add_test(NAME starlet_aurora_texture_writes COMMAND starlet_aurora_texture_writes_tests)
