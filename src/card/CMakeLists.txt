add_library(starlet_card STATIC
    card.cpp
    format.cpp
)
add_library(starlet::card ALIAS starlet_card)
target_include_directories(starlet_card PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_card PUBLIC cxx_std_17)
set_target_properties(starlet_card PROPERTIES CXX_EXTENSIONS OFF)
