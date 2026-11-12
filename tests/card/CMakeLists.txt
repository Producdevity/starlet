add_executable(starlet_card_tests card_tests.cpp)
target_link_libraries(starlet_card_tests PRIVATE starlet_card)
add_test(NAME starlet_card COMMAND starlet_card_tests "${CMAKE_CURRENT_BINARY_DIR}/card_tests_work")

# The SDK's CARD calls run on starlet::os, which is Linux only.
if(TARGET starlet_card_sdk)
    add_executable(starlet_card_sdk_tests sdk_card_tests.cpp)
    target_link_libraries(starlet_card_sdk_tests PRIVATE starlet::card_sdk)
    add_test(NAME starlet_card_sdk COMMAND starlet_card_sdk_tests)
    set_tests_properties(starlet_card_sdk PROPERTIES TIMEOUT 60)
endif()
