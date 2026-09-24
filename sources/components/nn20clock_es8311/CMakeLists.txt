# Espressif's ES8311 codec driver, vendored. FIRMWARE ONLY.
#
# This is a copy of the registry component with its I2C layer swapped -
# see VENDORED.md. It cannot be a managed dependency, because the
# published version uses the legacy I2C driver and ESP-IDF aborts at
# startup if that is linked alongside the i2c_master driver the display
# uses for touch.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS src/es8311.c
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_driver_gpio
)
