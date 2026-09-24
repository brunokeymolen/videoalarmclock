# The 720x720 panel and LVGL (design 11). FIRMWARE ONLY.
#
# ST7703 over MIPI-DSI plus LVGL: none of it exists on a desktop, so the
# component is left out of the host build rather than shimmed. The
# screens that draw on it (nn20clock_time_ui) are firmware-only for the
# same reason; their logic that is worth testing lives in dual-mode
# components instead.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS "src/nn20clock_display.c"
    INCLUDE_DIRS "include"
    # esp_driver_i2c is public, not private: the header hands out the
    # board's I2C bus handle so the audio codec can share it (design 2).
    REQUIRES nn20clock_platform nn20clock_worker esp_driver_i2c
    PRIV_REQUIRES driver esp_lcd esp_timer esp_mm esp_driver_ledc
)
