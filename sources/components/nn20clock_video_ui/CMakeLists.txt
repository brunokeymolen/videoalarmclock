# The ringing screen (design 11, VideoPlayerUi). FIRMWARE ONLY: it draws
# with LVGL and drives the player.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS src/nn20clock_video_ui.c
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_ui nn20clock_manager nn20clock_player nn20clock_timefmt nn20clock_media nn20clock_display
             nn20clock_fonts
             lvgl
)
