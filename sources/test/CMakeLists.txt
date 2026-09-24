# Host test suites. One executable per component, registered with ctest.
#
# Adding a component's suite: drop test_<component>.c in here and add the
# component's short name to NN20CLOCK_TEST_SUITES. The convention is that
# test_<name>.c links the nn20clock_<name> library.

set(NN20CLOCK_TEST_SUITES
    nn20clock_workers
    nn20clock_timer
    nn20clock_timefmt
    nn20clock_alarm
    nn20clock_media
    nn20clock_avi
    nn20clock_brightness
    nn20clock_ringbuf
    nn20clock_storage
    nn20clock_ui
    nn20clock_ota
    nn20clock_manager
    nn20clock_app
)

foreach(suite ${NN20CLOCK_TEST_SUITES})
    add_executable(test_${suite} test_${suite}.c)

    target_link_libraries(test_${suite} PRIVATE ${suite})

    target_include_directories(test_${suite} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_compile_options(test_${suite} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Werror
    )

    add_test(NAME ${suite} COMMAND test_${suite})
endforeach()
