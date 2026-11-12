add_executable(starlet_thp_video_tests video_tests.cpp)
target_link_libraries(starlet_thp_video_tests PRIVATE starlet::thp)
add_test(NAME starlet_thp_video COMMAND starlet_thp_video_tests)
set_tests_properties(starlet_thp_video PROPERTIES TIMEOUT 60)

# The same frames through the SDK's THPVideoDecode, which also reports the planes it wrote.
if(TARGET starlet_thp_sdk)
    add_executable(starlet_thp_sdk_tests video_tests.cpp)
    target_compile_definitions(starlet_thp_sdk_tests PRIVATE STARLET_THP_SDK_TESTS)
    target_link_libraries(starlet_thp_sdk_tests PRIVATE starlet::thp_sdk)
    add_test(NAME starlet_thp_sdk COMMAND starlet_thp_sdk_tests)
    set_tests_properties(starlet_thp_sdk PROPERTIES TIMEOUT 60)
endif()

add_executable(starlet_thp_resources_tests resources_tests.cpp)
target_link_libraries(starlet_thp_resources_tests PRIVATE starlet::thp)
add_test(NAME starlet_thp_resources COMMAND starlet_thp_resources_tests)
