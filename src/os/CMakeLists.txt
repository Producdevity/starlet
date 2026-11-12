# The console's OS for SDK-level ports: guest threads on the single emulated CPU, interrupts, alarms,
# time, arena and game memory, reports and crash reports. Linux, C++17.
find_package(Threads REQUIRED)
add_library(starlet_os STATIC
    crash_report.cpp
    game_memory.cpp
    guest_cpu.cpp
    host_allocation.cpp
    os_misc.cpp
    os_core.cpp
    profiler.cpp
)
add_library(starlet::os ALIAS starlet_os)
target_include_directories(starlet_os PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_os PUBLIC cxx_std_17)
set_target_properties(starlet_os PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(starlet_os PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
