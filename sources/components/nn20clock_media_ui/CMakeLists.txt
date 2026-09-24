# The media selection screen (design 11, MediaPlaybackUi). FIRMWARE ONLY
# - it draws with LVGL. What it lists comes from nn20clock_media and
# nn20clock_sd, and what it starts is nn20clock_player; both of those
# have their logic tested elsewhere, and there is nothing left here but
# widgets.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS "src/nn20clock_media_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_ui nn20clock_manager
             nn20clock_sd nn20clock_media nn20clock_fonts
)
