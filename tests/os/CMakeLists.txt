add_executable(starlet_os_tests guest_cpu_tests.cpp)
target_link_libraries(starlet_os_tests PRIVATE starlet::os)
add_test(NAME starlet_os_guest_cpu COMMAND starlet_os_tests)
set_tests_properties(starlet_os_guest_cpu PROPERTIES TIMEOUT 20)

# Linked without PIE so the image sits low, as in a port.
add_executable(starlet_os_game_memory_tests game_memory_tests.cpp)
target_link_libraries(starlet_os_game_memory_tests PRIVATE starlet::os)
target_compile_options(starlet_os_game_memory_tests PRIVATE -fno-pie)
target_link_options(starlet_os_game_memory_tests PRIVATE -no-pie)
add_test(NAME starlet_os_game_memory COMMAND starlet_os_game_memory_tests)

# Without PIE, as in a port: the report names functions from the image's symbol table.
add_executable(starlet_os_crash_report_tests crash_report_tests.cpp)
target_link_libraries(starlet_os_crash_report_tests PRIVATE starlet::os)
target_compile_options(starlet_os_crash_report_tests PRIVATE -fno-pie)
target_link_options(starlet_os_crash_report_tests PRIVATE -no-pie)
add_test(NAME starlet_os_crash_report COMMAND starlet_os_crash_report_tests)

# Its guest code keeps frame records, as a port compiles the game's code for asynchronous interrupts.
add_executable(starlet_os_async_interrupt_tests async_interrupt_tests.cpp)
target_link_libraries(starlet_os_async_interrupt_tests PRIVATE starlet::os)
target_compile_options(starlet_os_async_interrupt_tests PRIVATE -fno-omit-frame-pointer)
add_test(NAME starlet_os_async_interrupts COMMAND starlet_os_async_interrupt_tests)
set_tests_properties(starlet_os_async_interrupts PROPERTIES TIMEOUT 30)
