add_library(starlet_thp STATIC video.cpp resources.cpp)
add_library(starlet::thp ALIAS starlet_thp)
target_include_directories(starlet_thp PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_thp PUBLIC cxx_std_17)
set_target_properties(starlet_thp PROPERTIES CXX_EXTENSIONS OFF)

# The SDK's THP decoder calls, for ports that keep the SDK's player on starlet::os (Linux only). A
# port with its own player links starlet::thp alone.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(starlet_thp_sdk STATIC sdk_thp.cpp)
    add_library(starlet::thp_sdk ALIAS starlet_thp_sdk)
    target_link_libraries(starlet_thp_sdk PUBLIC starlet_thp starlet_os)
    set_target_properties(starlet_thp_sdk PROPERTIES CXX_EXTENSIONS OFF)
endif()
