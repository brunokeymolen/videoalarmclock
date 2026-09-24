# Wi-Fi and NTP (design 14). FIRMWARE ONLY.
#
# The ESP32-P4 has no radio: esp_wifi_remote forwards the ordinary
# esp_wifi API to the ESP32-C6 co-processor over SDIO, and esp_hosted is
# the transport. Neither has a host-side equivalent worth pretending to,
# so this component is absent from the host build rather than shimmed -
# see the note in nn20clock_platform.h about honest seams.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS "src/nn20clock_net.c"
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_worker
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip
)
