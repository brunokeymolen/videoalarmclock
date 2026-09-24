# Playing an alarm (design 16, 17 Milestone 7). FIRMWARE ONLY: the
# hardware JPEG decoder, the panel and the codec. The container parsing
# it drives is nn20clock_avi, and the read-ahead window it streams
# through is nn20clock_ringbuf; both are dual-mode and tested on the
# host.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS src/nn20clock_player.c
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_worker nn20clock_avi
             nn20clock_ringbuf
             nn20clock_audio nn20clock_display
    PRIV_REQUIRES esp_driver_jpeg esp_timer
)
