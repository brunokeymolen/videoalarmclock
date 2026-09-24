# The clock face (design 11, TimeUi). FIRMWARE ONLY - it draws with
# LVGL. The part worth testing without a board, formatting a time into
# "HH:MM", lives in nn20clock_timefmt and is dual-mode.

if(NOT ESP_PLATFORM)
    return()
endif()

# The big digits come from nn20clock_fonts, which the ringing screen
# draws with too. See that component's README.md.
idf_component_register(
    SRCS "src/nn20clock_time_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_ui nn20clock_timer nn20clock_timefmt
             nn20clock_player nn20clock_fonts
    PRIV_REQUIRES nn20clock_display
)
