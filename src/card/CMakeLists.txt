add_library(starlet_card STATIC
    card.cpp
    format.cpp
)
add_library(starlet::card ALIAS starlet_card)
target_include_directories(starlet_card PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_card PUBLIC cxx_std_17)
set_target_properties(starlet_card PROPERTIES CXX_EXTENSIONS OFF)

# The SDK's CARD calls for ports on starlet::os (Linux only).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(starlet_card_sdk STATIC sdk_card.cpp)
    add_library(starlet::card_sdk ALIAS starlet_card_sdk)
    target_link_libraries(starlet_card_sdk PUBLIC starlet_card starlet_os)
    set_target_properties(starlet_card_sdk PROPERTIES CXX_EXTENSIONS OFF)
endif()
