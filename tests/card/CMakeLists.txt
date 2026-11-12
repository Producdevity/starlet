add_executable(starlet_card_tests card_tests.cpp)
target_link_libraries(starlet_card_tests PRIVATE starlet_card)
add_test(NAME starlet_card COMMAND starlet_card_tests "${CMAKE_CURRENT_BINARY_DIR}/card_tests_work")
