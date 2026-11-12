add_executable(starlet_dvd_tests dvd_tests.cpp)
target_link_libraries(starlet_dvd_tests PRIVATE starlet::dvd)
add_test(NAME starlet_dvd COMMAND starlet_dvd_tests)
set_tests_properties(starlet_dvd PROPERTIES TIMEOUT 20)
