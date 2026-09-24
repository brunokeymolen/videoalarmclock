# ClockManager (design 5, 8). Dual-mode: an IDF component on the target,
# a static library on the host.
#
# It depends on nn20clock_ui, but not the other way round: UiBase only
# forward-declares NN20ClockManager and reaches it through a callback, so
# the two headers do not form a cycle.

set(MANAGER_SRCS
    src/nn20clock_manager.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MANAGER_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform nn20clock_worker nn20clock_timer
                 nn20clock_storage nn20clock_ui
        PRIV_REQUIRES nn20clock_reqpool
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_manager STATIC ${MANAGER_SRCS})

target_include_directories(nn20clock_manager
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_manager
    PUBLIC  nn20clock_platform nn20clock_worker nn20clock_timer
            nn20clock_storage nn20clock_ui
    PRIVATE nn20clock_reqpool
)

target_compile_options(nn20clock_manager PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
