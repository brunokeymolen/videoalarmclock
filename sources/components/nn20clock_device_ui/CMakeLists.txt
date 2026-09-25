# The system settings screen (design 11, DeviceSettingsUi). FIRMWARE
# ONLY - it draws with LVGL and drives the radio.
#
# It reuses the bold font generated for the alarm screen rather than
# shipping a second copy; tools/gen-fonts.sh builds it.
#
# nn20clock_ota is a REQUIRES rather than a PRIV_REQUIRES: the ctor
# takes an NN20ClockOta* and the app root has to be able to name it.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS "src/nn20clock_device_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_ui nn20clock_manager
             nn20clock_storage nn20clock_net nn20clock_ota
    PRIV_REQUIRES nn20clock_display nn20clock_brightness nn20clock_alarm_ui
                  nn20clock_timefmt
                  esp_app_format
)
