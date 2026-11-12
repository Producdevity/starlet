# The DVD SDK API over installed disc trees, for SDK-level ports. Linux, C++17.
add_library(starlet_dvd STATIC dvd.cpp)
add_library(starlet::dvd ALIAS starlet_dvd)
target_include_directories(starlet_dvd PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_dvd PUBLIC cxx_std_17)
set_target_properties(starlet_dvd PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(starlet_dvd PUBLIC starlet::os starlet::disc)
