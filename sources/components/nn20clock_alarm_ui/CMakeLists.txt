# The alarm settings screen (design 11). FIRMWARE ONLY - it draws with
# LVGL. The logic worth testing without a board lives in
# nn20clock_alarm (when an alarm fires) and nn20clock_timefmt (what the
# text says), both dual-mode with their own suites.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS "src/nn20clock_alarm_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_ui nn20clock_manager
             nn20clock_alarm nn20clock_timefmt nn20clock_sd
             nn20clock_media nn20clock_fonts
    PRIV_REQUIRES nn20clock_display
)
