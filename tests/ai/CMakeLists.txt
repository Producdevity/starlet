# The AI model with a hand-driven guest CPU and output, so the tests build on any host.
add_executable(starlet_ai_tests
    ai_tests.cpp
    fake_ai_platform.cpp
    "${PROJECT_SOURCE_DIR}/src/ai/ai.cpp"
)
target_include_directories(starlet_ai_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(starlet_ai_tests PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(starlet_ai_tests PRIVATE Threads::Threads)
add_test(NAME starlet_ai COMMAND starlet_ai_tests)
set_tests_properties(starlet_ai PROPERTIES TIMEOUT 30)
