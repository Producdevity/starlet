# AX on the real AI model, with a hand-driven guest CPU and output, so the tests build on any host.
add_executable(starlet_ax_tests
    ax_tests.cpp
    fake_ax_platform.cpp
    ../ai/fake_ai_platform.cpp
    "${PROJECT_SOURCE_DIR}/src/ai/ai.cpp"
    "${PROJECT_SOURCE_DIR}/src/ax/ax_sdk.cpp"
    "${PROJECT_SOURCE_DIR}/src/ax/dsp.cpp"
)
target_include_directories(starlet_ax_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_ax_tests PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(starlet_ax_tests PRIVATE Threads::Threads)
add_test(NAME starlet_ax COMMAND starlet_ax_tests)
set_tests_properties(starlet_ax PROPERTIES TIMEOUT 60)
