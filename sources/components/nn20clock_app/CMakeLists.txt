# The composition root (design 4's CoreWorker). Dual-mode: an IDF
# component on the target, a static library on the host.
#
# This is the only component that depends on all the others, and nothing
# depends on it except app_main and its test. Keep it that way - a
# component that reaches back into the app root has its dependencies
# upside down.

set(APP_SRCS
    src/nn20clock_app.c
)

if(ESP_PLATFORM)
    # The firmware build adds the hardware-bound services and the
    # screens; the host build below has neither and runs headless.
    idf_component_register(
        SRCS ${APP_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform nn20clock_workers nn20clock_storage
                 nn20clock_brightness
                 nn20clock_timer nn20clock_manager
                 nn20clock_display nn20clock_net nn20clock_sd nn20clock_ftp
                 nn20clock_ota
                 nn20clock_media nn20clock_audio nn20clock_player
        PRIV_REQUIRES nn20clock_time_ui nn20clock_alarm_ui nn20clock_device_ui
                      nn20clock_video_ui nn20clock_media_ui
                      nn20clock_media_mgmt_ui
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_app STATIC ${APP_SRCS})

target_include_directories(nn20clock_app
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_app
    PUBLIC nn20clock_platform nn20clock_workers nn20clock_storage
           nn20clock_brightness
           nn20clock_timer nn20clock_manager
)

target_compile_options(nn20clock_app PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
