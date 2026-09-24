# The ES8311 codec and I2S output (design 7, 13). FIRMWARE ONLY: a codec
# on an I2C bus and this board's pin numbers, none of which exist on a
# desktop. The container parsing that decides what gets played is in
# nn20clock_avi, which is tested on the host.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS src/nn20clock_audio.c
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_es8311 nn20clock_display
             esp_driver_i2s esp_driver_gpio
    PRIV_REQUIRES esp_timer
)
