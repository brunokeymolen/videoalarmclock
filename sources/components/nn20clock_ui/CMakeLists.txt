# UiBase (design 11). Dual-mode: an IDF component on the target, a static
# library on the host.
#
# Note what is NOT here: LVGL, the display driver, and the touch driver.
# The base class only routes calls onto the UiWorker, which is pure logic
# and therefore testable on the host. The concrete screens that draw
# (Milestone 2 onward) will need LVGL and will be firmware-only.

set(UI_SRCS
    src/nn20clock_ui.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${UI_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform nn20clock_worker nn20clock_timer
                 nn20clock_reqpool
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_ui STATIC ${UI_SRCS})

target_include_directories(nn20clock_ui
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# All four are public: the base struct that screens embed exposes the
# worker handle, the timer event type, and the request pool.
target_link_libraries(nn20clock_ui
    PUBLIC nn20clock_platform nn20clock_worker nn20clock_timer
           nn20clock_reqpool
)

target_compile_options(nn20clock_ui PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
